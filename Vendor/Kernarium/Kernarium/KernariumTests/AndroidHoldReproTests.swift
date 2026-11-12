// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
@testable import Kernarium

/// The Android guest's edge into the background, driven on a Mac: the
/// application is the test host, its guest is running, and a script outside it
/// (`Kernarium/scripts/repro-android-hold-mac.py`) asks for the edges and
/// watches the guest through adb. It does nothing unless that script names the
/// directory it talks through, so an ordinary run of the suite skips it.
///
/// The test drives the real `BackgroundController` with the real effects, the
/// GPU hold, the power key and the display query among them; only the job is
/// claimed, since the system's continued task cannot be had on a Mac. A Mac
/// never revokes the GPU, so what it shows is the guest's side of the edge.
///
/// The script asks by creating `edge-background`, `edge-foreground` or
/// `finish` in the directory under Documents, and the test answers with
/// `ready` once the guest runs and `<edge>.done` after each edge, holding what
/// the effects did and when.
@MainActor
struct AndroidHoldReproTests {
    static let directoryKey = "KERNARIUM_HOLD_REPRO_DIR"
    static let secondsKey = "KERNARIUM_HOLD_REPRO_SECONDS"

    nonisolated static var directoryName: String? {
        ProcessInfo.processInfo.environment[directoryKey]
    }

    /// The real effects with a job claimed, and a log of what the policy asked
    /// for and when, in milliseconds from the edge it handled.
    private final class ReproEffects: BackgroundEffects {
        /// What was asked for and when. The hold can be taken from the
        /// bound's own thread, so the log takes a lock.
        private nonisolated final class EdgeLog: @unchecked Sendable {
            private let lock = NSLock()
            private var recorded: [[String]] = []
            private var origin = DispatchTime.now()

            var calls: [[String]] { lock.withLock { recorded } }

            func start() {
                lock.withLock {
                    recorded.removeAll()
                    origin = .now()
                }
            }

            func record(_ call: String) {
                lock.withLock {
                    let elapsed = DispatchTime.now().uptimeNanoseconds -
                        origin.uptimeNanoseconds
                    recorded.append([String(elapsed / 1_000_000), call])
                }
            }
        }

        /// The real gate, with the time the hold is taken or given back.
        private nonisolated struct RecordingRuntimeGate: GuestGPUGate {
            let log: EdgeLog
            private let runtime = RuntimeGPUGate()

            init(_ log: EdgeLog) {
                self.log = log
            }

            func hold() {
                log.record("hold")
                runtime.hold()
            }

            func release() {
                log.record("release")
                runtime.release()
            }
        }

        private let real: SystemBackgroundEffects
        private let log = EdgeLog()
        let gpuHold: GuestGPUHold

        var calls: [[String]] { log.calls }

        init() {
            real = SystemBackgroundEffects(
                persistence: .shared,
                pictureInPicture: nil
            )
            gpuHold = GuestGPUHold(gate: RecordingRuntimeGate(log))
        }

        func startEdge() {
            log.start()
        }

        private func record(_ call: String) {
            log.record(call)
        }

        func flushGuest(until deadline: DispatchTime?) {
            record("flush")
            real.flushGuest(until: deadline)
        }
        func quiesceGuest(reason: String, until deadline: DispatchTime?) {
            record("quiesce")
            real.quiesceGuest(reason: reason, until: deadline)
        }
        func resumeGuest() {
            record("resume")
            real.resumeGuest()
        }
        func setPageCacheTrimming(_ enabled: Bool) {
            record("trim \(enabled ? "on" : "off")")
            real.setPageCacheTrimming(enabled)
        }
        func setBackgroundThrottle(_ enabled: Bool) {
            real.setBackgroundThrottle(enabled)
        }
        var jobActive: Bool { true }
        var guestIsAndroid: Bool { real.guestIsAndroid }
        func pressPowerKey() {
            record("power")
            real.pressPowerKey()
        }
        var guestDisplayActive: Bool? { real.guestDisplayActive }
        func waitForGuestDisplayOff(until deadline: DispatchTime) -> Bool {
            record("wait display off")
            let off = real.waitForGuestDisplayOff(until: deadline)
            record(off ? "display off" : "display still on")
            return off
        }
        var automationHoldDelay: TimeInterval { real.automationHoldDelay }
        var pictureInPictureCanStart: Bool { false }
        var pictureInPictureActive: Bool { false }
        func stopPictureInPicture() {}
        func keepScheduled(named name: String) -> any BackgroundCancellable {
            record("begin \(name)")
            return real.keepScheduled(named: name)
        }
        func schedule(
            after seconds: TimeInterval,
            _ action: @escaping @MainActor () -> Void
        ) -> any BackgroundCancellable {
            real.schedule(after: seconds) { [weak self] in
                self?.record("scheduled action")
                action()
            }
        }
    }

    @Test(.enabled(if: AndroidHoldReproTests.directoryName != nil))
    func theScriptDrivesTheEdgesOfAnAndroidGuest() async throws {
        let name = try #require(Self.directoryName)
        let documents = try #require(
            FileManager.default.urls(
                for: .documentDirectory,
                in: .userDomainMask
            ).first
        )
        let directory = documents.appendingPathComponent(name, isDirectory: true)
        let fileManager = FileManager.default
        try? fileManager.removeItem(at: directory)
        try fileManager.createDirectory(
            at: directory,
            withIntermediateDirectories: true
        )
        let seconds = ProcessInfo.processInfo.environment[Self.secondsKey]
            .flatMap(Double.init) ?? 1500
        let deadline = Date().addingTimeInterval(seconds)

        func exists(_ file: String) -> Bool {
            fileManager.fileExists(
                atPath: directory.appendingPathComponent(file).path
            )
        }
        func consume(_ file: String) {
            try? fileManager.removeItem(
                at: directory.appendingPathComponent(file)
            )
        }
        func write(_ file: String, _ contents: String) {
            try? contents.write(
                to: directory.appendingPathComponent(file),
                atomically: true,
                encoding: .utf8
            )
        }

        // The application starts the guest itself when its view has a size.
        while !(EmulatorLauncher.shared.phase == .running &&
            EmulatorLauncher.shared.runningGuestIsAndroid) {
            try #require(Date() < deadline, "the Android guest never ran")
            try await Task.sleep(for: .milliseconds(200))
        }
        write("ready", "running")

        let effects = ReproEffects()
        let controller = BackgroundController(
            effects: effects,
            mode: { .keepRunning }
        )
        while !exists("finish") {
            try #require(Date() < deadline, "the script never finished")
            if exists("edge-background") {
                consume("edge-background")
                effects.startEdge()
                let started = DispatchTime.now()
                controller.didEnterBackground()
                report(
                    "edge-background", started, effects, controller,
                    directory, write
                )
            } else if exists("edge-foreground") {
                consume("edge-foreground")
                effects.startEdge()
                let started = DispatchTime.now()
                controller.willEnterForeground()
                report(
                    "edge-foreground", started, effects, controller,
                    directory, write
                )
            }
            try await Task.sleep(for: .milliseconds(100))
        }
        consume("finish")
        write("finished", "done")
    }

    private func report(
        _ edge: String,
        _ started: DispatchTime,
        _ effects: ReproEffects,
        _ controller: BackgroundController,
        _ directory: URL,
        _ write: (String, String) -> Void
    ) {
        let handler = DispatchTime.now().uptimeNanoseconds -
            started.uptimeNanoseconds
        let payload: [String: Any] = [
            "edge": edge,
            "handler_ms": Int(handler / 1_000_000),
            "phase": "\(controller.phase)",
            "calls": effects.calls,
        ]
        let data = (try? JSONSerialization.data(
            withJSONObject: payload,
            options: [.sortedKeys]
        )) ?? Data()
        write("\(edge).done", String(decoding: data, as: UTF8.self))
    }
}
