// SPDX-License-Identifier: GPL-2.0-or-later
import UIKit
import XCTest

/// A long-lived UI automation host for a connected device.
///
/// Each XCTest session that turns UI automation on can make the device ask
/// for the owner's passcode or biometrics. This test keeps one session alive
/// and executes commands a Mac places in the runner's data container, so one
/// approval serves many unattended commands. `Kernarium/scripts/ipad-automation.py`
/// starts it, sends the commands, and reads the results.
///
/// The channel lives under `Documents/automation` in the runner's container:
///
/// - `inbox/<id>.json` holds a command. The host acts on it only once
///   `inbox/<id>.ready` also exists, and waits while the command does not
///   parse yet, so a command copied together with its marker is never read
///   half written.
/// - `outbox/<id>.json` receives the result, and `outbox/<id>.png` a screenshot.
/// - `state.json` is a heartbeat, rewritten every two seconds from a
///   background queue, so a command that blocks the main thread still shows
///   the host alive and names the command it is running.
///
/// The test is skipped unless `KERNARIUM_AUTOMATION_HOST=1`.
final class AutomationHost: XCTestCase {
    private static let heartbeatInterval: TimeInterval = 2
    private static let pollInterval: TimeInterval = 0.1
    private static let outboxRetention: TimeInterval = 3600

    /// Failures XCTest raises inside a command, such as a tap on an element
    /// that vanished, become the command's error instead of ending the host.
    private let issues = IssueSink()

    override func setUpWithError() throws {
        continueAfterFailure = true
    }

    override func record(_ issue: XCTIssue) {
        if !issues.capture(issue.compactDescription) {
            super.record(issue)
        }
    }

    @MainActor
    func testServe() throws {
        let environment = ProcessInfo.processInfo.environment
        try XCTSkipUnless(
            environment["KERNARIUM_AUTOMATION_HOST"] == "1",
            "the automation host runs only when KERNARIUM_AUTOMATION_HOST=1"
        )
        let lifetime = TimeInterval(
            environment["KERNARIUM_AUTOMATION_HOST_SECONDS"] ?? ""
        ) ?? 8 * 3600
        executionTimeAllowance = lifetime + 600

        let channel = try Channel(
            root: URL(fileURLWithPath: NSHomeDirectory())
                .appendingPathComponent("Documents/automation", isDirectory: true)
        )
        let state = HostState(
            nonce: environment["KERNARIUM_AUTOMATION_NONCE"] ?? "",
            deadline: Date().addingTimeInterval(lifetime)
        )
        let heartbeatQueue = DispatchQueue(label: "automation-host.heartbeat")
        let heartbeat = DispatchSource.makeTimerSource(queue: heartbeatQueue)
        heartbeat.schedule(deadline: .now(), repeating: Self.heartbeatInterval)
        heartbeat.setEventHandler { channel.writeState(state.snapshot()) }
        heartbeat.resume()
        defer {
            heartbeat.cancel()
            state.finish()
            // Queued after any beat in flight, so the last state says finished.
            heartbeatQueue.sync { channel.writeState(state.snapshot()) }
        }

        var executor = Executor(issues: issues)
        var lastPrune = Date.distantPast
        while Date() < state.deadline {
            for id in channel.readyCommands() {
                let result: [String: Any]
                var stop = false
                switch channel.takeCommand(id) {
                case .pending:
                    continue
                case .unreadable:
                    result = ["ok": false, "error": "the command is not a JSON object"]
                case let .command(command):
                    state.begin(id)
                    result = executor.run(command, id: id, channel: channel)
                    state.end()
                    stop = command["op"] as? String == "stop"
                }
                channel.writeResult(id, result)
                if stop {
                    return
                }
            }
            if Date().timeIntervalSince(lastPrune) > 60 {
                channel.pruneOutbox(olderThan: Self.outboxRetention)
                lastPrune = Date()
            }
            RunLoop.current.run(until: Date().addingTimeInterval(Self.pollInterval))
        }
    }
}

/// Collects XCTest issues while a command runs.
private final class IssueSink: @unchecked Sendable {
    private let lock = NSLock()
    private var active = false
    private var messages: [String] = []

    func begin() {
        lock.withLock {
            active = true
            messages = []
        }
    }

    func end() -> [String] {
        lock.withLock {
            active = false
            return messages
        }
    }

    /// Returns whether the issue was taken by a running command.
    func capture(_ message: String) -> Bool {
        lock.withLock {
            guard active else { return false }
            messages.append(message)
            return true
        }
    }
}

/// What the heartbeat reports.
private final class HostState: @unchecked Sendable {
    let nonce: String
    let deadline: Date
    private let started = Date()
    private let lock = NSLock()
    private var command: String?
    private var commandStarted: Date?
    private var served = 0
    private var finished = false

    init(nonce: String, deadline: Date) {
        self.nonce = nonce
        self.deadline = deadline
    }

    func begin(_ id: String) {
        lock.withLock {
            command = id
            commandStarted = Date()
        }
    }

    func end() {
        lock.withLock {
            command = nil
            commandStarted = nil
            served += 1
        }
    }

    func finish() {
        lock.withLock { finished = true }
    }

    func snapshot() -> [String: Any] {
        lock.withLock {
            var state: [String: Any] = [
                "nonce": nonce,
                "pid": Int(getpid()),
                "started": started.timeIntervalSince1970,
                "time": Date().timeIntervalSince1970,
                "deadline": deadline.timeIntervalSince1970,
                "served": served,
                "finished": finished,
            ]
            if let command, let commandStarted {
                state["command"] = command
                state["commandStarted"] = commandStarted.timeIntervalSince1970
            }
            return state
        }
    }
}

/// The file queue in the runner's data container.
private final class Channel: @unchecked Sendable {
    let inbox: URL
    let outbox: URL
    private let state: URL
    private let files = FileManager.default
    /// When the host first saw the marker of a command that did not parse.
    private var firstSeen: [String: Date] = [:]

    init(root: URL) throws {
        inbox = root.appendingPathComponent("inbox", isDirectory: true)
        outbox = root.appendingPathComponent("outbox", isDirectory: true)
        state = root.appendingPathComponent("state.json")
        for directory in [inbox, outbox] {
            try files.createDirectory(at: directory, withIntermediateDirectories: true)
        }
        // Commands left from an earlier host were meant for that host.
        for name in (try? files.contentsOfDirectory(atPath: inbox.path)) ?? [] {
            try? files.removeItem(at: inbox.appendingPathComponent(name))
        }
    }

    /// Command identifiers whose ready marker has arrived, oldest first.
    func readyCommands() -> [String] {
        // Another tool may clear the container, directories included.
        try? files.createDirectory(at: inbox, withIntermediateDirectories: true)
        let names = (try? files.contentsOfDirectory(atPath: inbox.path)) ?? []
        return names.filter { $0.hasSuffix(".ready") }
            .map { String($0.dropLast(".ready".count)) }
            .sorted()
    }

    enum Taken {
        case pending
        case unreadable
        case command([String: Any])
    }

    /// Takes a command out of the inbox. A command that does not parse yet
    /// may still be arriving; it is given ten seconds from its marker.
    func takeCommand(_ id: String) -> Taken {
        let command = inbox.appendingPathComponent("\(id).json")
        let marker = inbox.appendingPathComponent("\(id).ready")
        let parsed = (try? Data(contentsOf: command)).flatMap {
            (try? JSONSerialization.jsonObject(with: $0)) as? [String: Any]
        }
        if parsed == nil {
            let seen = firstSeen[id] ?? Date()
            firstSeen[id] = seen
            if Date().timeIntervalSince(seen) < 10 {
                return .pending
            }
        }
        firstSeen[id] = nil
        try? files.removeItem(at: command)
        try? files.removeItem(at: marker)
        return parsed.map { .command($0) } ?? .unreadable
    }

    func attachmentURL(_ id: String, extension ext: String) -> URL {
        outbox.appendingPathComponent("\(id).\(ext)")
    }

    func writeResult(_ id: String, _ result: [String: Any]) {
        write(result, to: outbox.appendingPathComponent("\(id).json"))
    }

    func writeState(_ snapshot: [String: Any]) {
        write(snapshot, to: state)
    }

    func pruneOutbox(olderThan age: TimeInterval) {
        let cutoff = Date().addingTimeInterval(-age)
        let names = (try? files.contentsOfDirectory(atPath: outbox.path)) ?? []
        for name in names {
            let url = outbox.appendingPathComponent(name)
            let modified = (try? files.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date
            if let modified, modified < cutoff {
                try? files.removeItem(at: url)
            }
        }
    }

    private func write(_ object: [String: Any], to url: URL) {
        guard let data = try? JSONSerialization.data(
            withJSONObject: object, options: [.sortedKeys]
        ) else { return }
        try? files.createDirectory(
            at: url.deletingLastPathComponent(), withIntermediateDirectories: true
        )
        try? data.write(to: url, options: .atomic)
    }
}

/// Runs one command against the device's user interface.
@MainActor
private struct Executor {
    let issues: IssueSink
    private var applications: [String: XCUIApplication] = [:]

    init(issues: IssueSink) {
        self.issues = issues
    }

    private struct Failure: Error {
        let message: String
    }

    /// Runs one command. Nothing a command does ends the host: a thrown
    /// error, an XCTest issue, and an Objective-C exception XCTest raises all
    /// become the command's error. An exception left to unwind the serve
    /// loop would end the test, and with it the host.
    mutating func run(_ command: [String: Any], id: String, channel: Channel) -> [String: Any] {
        issues.begin()
        var result: [String: Any] = [:]
        let exception = KernariumAutomationCatchException {
            do {
                result = try perform(command, id: id, channel: channel)
                result["ok"] = true
            } catch let failure as Failure {
                result = ["ok": false, "error": failure.message]
            } catch {
                result = ["ok": false, "error": "\(error)"]
            }
        }
        if let exception {
            result = [
                "ok": false,
                "error": "XCTest raised \(exception.name.rawValue): \(exception.reason ?? "no reason")",
            ]
        }
        let raised = issues.end()
        if !raised.isEmpty {
            result["ok"] = false
            result["issues"] = raised
            if result["error"] == nil {
                result["error"] = raised.joined(separator: "; ")
            }
        }
        // Issues a command raises are taken from XCTest's record, so the
        // host's log is the only place a failure stays visible.
        if result["ok"] as? Bool != true {
            print("AutomationHost: \(command["op"] as? String ?? "?") \(id) failed: \(result["error"] ?? "")")
        }
        return result
    }

    private mutating func application(_ command: [String: Any]) -> XCUIApplication {
        let bundle = command["bundle"] as? String ?? "com.imxieyi.Kernarium"
        if let app = applications[bundle] {
            return app
        }
        let app = XCUIApplication(bundleIdentifier: bundle)
        applications[bundle] = app
        return app
    }

    private func timeout(_ command: [String: Any], default value: TimeInterval) -> TimeInterval {
        (command["timeout"] as? Double) ?? value
    }

    private func number(_ command: [String: Any], _ key: String) throws -> Double {
        guard let value = command[key] as? Double else {
            throw Failure(message: "the command has no number \(key)")
        }
        return value
    }

    /// A point given as an offset in points from the application's frame
    /// origin; springboard's frame is the whole screen.
    private func point(_ app: XCUIApplication, x: Double, y: Double) -> XCUICoordinate {
        app.coordinate(withNormalizedOffset: .zero).withOffset(CGVector(dx: x, dy: y))
    }

    private static func orientation(named name: String) -> UIDeviceOrientation? {
        switch name {
        case "portrait": .portrait
        case "portraitUpsideDown": .portraitUpsideDown
        case "landscapeLeft": .landscapeLeft
        case "landscapeRight": .landscapeRight
        default: nil
        }
    }

    private static func name(of orientation: UIDeviceOrientation) -> String {
        switch orientation {
        case .portrait: "portrait"
        case .portraitUpsideDown: "portraitUpsideDown"
        case .landscapeLeft: "landscapeLeft"
        case .landscapeRight: "landscapeRight"
        case .faceUp: "faceUp"
        case .faceDown: "faceDown"
        default: "unknown"
        }
    }

    /// The element a command names by accessibility identifier or label.
    private func element(_ command: [String: Any], in app: XCUIApplication) throws -> XCUIElement {
        let query = app.descendants(matching: .any)
        if let identifier = command["id"] as? String {
            return query.matching(identifier: identifier).firstMatch
        }
        if let label = command["label"] as? String {
            return query.matching(NSPredicate(format: "label == %@", label)).firstMatch
        }
        throw Failure(message: "the command names no element (id or label)")
    }

    private mutating func perform(
        _ command: [String: Any],
        id: String,
        channel: Channel
    ) throws -> [String: Any] {
        let op = command["op"] as? String ?? ""
        switch op {
        case "ping", "stop":
            return [:]

        case "launch":
            let app = application(command)
            app.launchArguments = command["arguments"] as? [String] ?? []
            app.launchEnvironment = command["environment"] as? [String: String] ?? [:]
            app.launch()
            return ["state": app.state.rawValue]

        case "activate":
            let app = application(command)
            app.activate()
            return ["state": app.state.rawValue]

        case "terminate":
            let app = application(command)
            app.terminate()
            return ["state": app.state.rawValue]

        case "state":
            return ["state": application(command).state.rawValue]

        case "tap":
            let app = application(command)
            if let x = command["x"] as? Double, let y = command["y"] as? Double {
                point(app, x: x, y: y).tap()
                return [:]
            }
            let target = try element(command, in: app)
            guard target.waitForExistence(timeout: timeout(command, default: 5)) else {
                throw Failure(message: "no such element")
            }
            target.tap()
            return ["label": target.label]

        case "drag":
            // Presses, drags at the given speed, then holds before lifting;
            // a held start is what picks up a window's resize handle.
            let app = application(command)
            let start = point(app, x: try number(command, "x1"), y: try number(command, "y1"))
            let end = point(app, x: try number(command, "x2"), y: try number(command, "y2"))
            let velocity = (command["velocity"] as? Double)
                .map { XCUIGestureVelocity(rawValue: CGFloat($0)) } ?? .default
            start.press(
                forDuration: command["press"] as? Double ?? 0.5,
                thenDragTo: end,
                withVelocity: velocity,
                thenHoldForDuration: command["hold"] as? Double ?? 0
            )
            return [:]

        case "orientation":
            // Without a name the command only reports the orientation.
            if let name = command["orientation"] as? String {
                guard let orientation = Self.orientation(named: name) else {
                    throw Failure(message: "unknown orientation \(name)")
                }
                XCUIDevice.shared.orientation = orientation
            }
            return ["orientation": Self.name(of: XCUIDevice.shared.orientation)]

        case "siri":
            // A typed request starts a real Siri session, which interrupts
            // the audio session of the application under test.
            guard let text = command["text"] as? String, !text.isEmpty else {
                throw Failure(message: "the command names no Siri request")
            }
            XCUIDevice.shared.siriService.activate(voiceRecognitionText: text)
            return [:]

        case "wait":
            let app = application(command)
            let target = try element(command, in: app)
            let limit = timeout(command, default: 10)
            let present = command["gone"] as? Bool == true
                ? target.waitForNonExistence(timeout: limit)
                : target.waitForExistence(timeout: limit)
            guard present else {
                throw Failure(message: "the element did not \(command["gone"] as? Bool == true ? "disappear" : "appear")")
            }
            return [:]

        case "exists":
            let app = application(command)
            let target = try element(command, in: app)
            return ["exists": target.exists]

        case "type":
            let text = command["text"] as? String ?? ""
            application(command).typeText(text)
            return [:]

        case "key":
            guard let key = command["key"] as? String else {
                throw Failure(message: "the command names no key")
            }
            // typeKey raises for anything but one UTF-16 code unit.
            guard key.utf16.count == 1 else {
                throw Failure(message: "a key is one UTF-16 character, not \(key.debugDescription); type text with the type command")
            }
            var flags: XCUIElement.KeyModifierFlags = []
            for name in command["modifiers"] as? [String] ?? [] {
                switch name {
                case "shift": flags.insert(.shift)
                case "control": flags.insert(.control)
                case "option": flags.insert(.option)
                case "command": flags.insert(.command)
                default: throw Failure(message: "unknown modifier \(name)")
                }
            }
            // The script resolves key names to characters XCTest delivers.
            application(command).typeKey(key, modifierFlags: flags)
            return [:]

        case "tree":
            return ["tree": application(command).debugDescription]

        case "screenshot":
            let png = XCUIScreen.main.screenshot().pngRepresentation
            let url = channel.attachmentURL(id, extension: "png")
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true
            )
            try png.write(to: url, options: .atomic)
            return ["file": url.lastPathComponent]

        default:
            throw Failure(message: "unknown op \(op)")
        }
    }
}
