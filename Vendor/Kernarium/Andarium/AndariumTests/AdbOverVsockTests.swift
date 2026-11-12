// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
@preconcurrency import Security
import Testing
import UIKit
@testable import Andarium

/// The application's own adb client against a real Android guest, with
/// authorization on. It runs only when the launch asks, as the hold test does:
///
///     TEST_RUNNER_ANDARIUM_ADB_TESTS=1 TEST_RUNNER_ANDARIUM_START=normal \
///         xcodebuild test -scheme Andarium -destination 'platform=macOS,variant=Designed for iPad' \
///         -only-testing:AndariumTests/AdbOverVsockTests
///
/// The application boots the guest as it launches; the tests wait for Android
/// and use the key the application made for that boot.
///
/// It checks a shell command, the multitouch features Android advertises to
/// applications, two runtime touch contacts reaching Android's input reader
/// through movement and independent release, the USB debugging setting and the
/// channel with it on and off, a computer's connection through the bridge, a
/// bug report (a zip above 1 MB, and its time), a cancel, a system APK pulled
/// from the image and installed as an update (and its time), the image's key
/// rule (the application's key accepted whatever the debugging setting; a
/// foreign key refused with no prompt and nothing queued while debugging is
/// off, and prompting while it is on), the host's time zone in the boot request
/// (`theGuestIsGivenTheHostsTimeZone`; `ANDARIUM_TIME_ZONE_APPLIED=1` also
/// waits for the guest to apply it), and the key after Erase guest. The tests
/// read the key the application made for the boot, again on every attempt until
/// adbd accepts it, and never make one: a key made before an `ANDARIUM_ERASE=1`
/// launch's erase deletes it would be one the guest was not given.
///
/// `aPromptWhoseDialogDiedIsRaisedAgain` kills SystemUI while the dialog is up
/// and waits for the dialog to come back without a new offer: stock Android
/// leaves adbd's prompt pending, so it needs an image whose framework starts
/// the dialog again. `aPromptWhoseDialogIsUpIsNotRaisedTwice` holds a dialog
/// up for 50 s and requires that the framework's look leaves it alone (one
/// activity record throughout). Both run only with
/// `ANDARIUM_ADB_REARM_TESTS=1`.
///
/// `androidReportsExactStorageSizes` (`ANDARIUM_STORAGE_TESTS=1`) installs the
/// storage test APK through the application's client, sets the guest's
/// `hidden_api_policy` to 1 for the run (the test calls one hidden API) and
/// restores it, runs `StorageAccountingTest#exactFilesystemBytes`, which checks
/// both primary-storage helpers and `StorageStatsManager` against the mounted
/// filesystem sizes, removes the APK and powers Android off cleanly.
/// `storageSettingsShowNonnegativeUsage` opens Android's internal Storage
/// settings, waits for the summary, checks for negative displayed values while
/// scrolling through the category rows, saves PNG screenshots and UI
/// hierarchies to the application's `tmp/StorageSettings/`, and powers Android
/// off.
@Suite(.serialized)
struct AdbOverVsockTests {
    private static var enabled: Bool { ProcessInfo.processInfo.environment["ANDARIUM_ADB_TESTS"] == "1" }

    /// The same code the screens run, for the key of this boot, once Android
    /// has booted and adbd answers (up to 15 minutes).
    ///
    /// The key is the guest's own: the application makes it as the start
    /// begins, and Erase guest, which an automation launch (`ANDARIUM_ERASE`)
    /// runs first, deletes the one before it. A test that made a key of its
    /// own, or read the old one, would hold a key the guest was not given and
    /// be refused for as long as it kept it. So the key is read again on every
    /// attempt and never made here; the one adbd accepts is the one returned.
    private func readyOperations() async throws -> AdbGuestOperations {
        let store = AppEntry.fromArguments().adbKeyStore
        let deadline = ContinuousClock.now + .seconds(15 * 60)
        var lastError = "none"
        var lastReport = ContinuousClock.now
        while ContinuousClock.now < deadline {
            do {
                if let identity = try store.existingIdentity() {
                    let operations = AdbGuestOperations(identity: identity, clock: GuestRunClock(), device: "device")
                    let booted = try await operations.shell("getprop sys.boot_completed")
                    if booted.text.hasPrefix("1") { return operations }
                    lastError = "boot_completed reads \(booted.text.debugDescription)"
                } else {
                    lastError = "the application has not made the guest's key yet"
                }
            } catch {
                lastError = String(describing: error)
            }
            if ContinuousClock.now - lastReport > .seconds(30) {
                print("ADBVSOCK: waiting for Android: \(lastError)")
                lastReport = ContinuousClock.now
            }
            try await Task.sleep(for: .seconds(5))
        }
        Issue.record("Android did not finish booting in 15 minutes; last: \(lastError)")
        throw CancellationError()
    }

    @Test(.timeLimit(.minutes(20))) func theClientReachesAdbdAndRunsACommand() async throws {
        guard Self.enabled else { return }
        let operations = try await readyOperations()
        let result = try await operations.shell("getprop ro.build.version.sdk; echo done")
        #expect(result.exitCode == 0)
        #expect(result.text.hasSuffix("done\n"))
        // The features the client asks for are the ones adbd has.
        let session = AdbSession(
            connector: VsockConnector(), signer: operations.identity?.signer, clock: operations.clock)
        let banner = try session.run { $0.banner }
        #expect(banner.features.isSuperset(of: ["shell_v2", "cmd", "stat_v2", "abb_exec"]))
    }

    /// The boot request carries the host's time zone. With
    /// `ANDARIUM_TIME_ZONE_APPLIED=1` (an image whose time zone script is
    /// there) the guest has also taken it, with "Set time zone automatically"
    /// at its default.
    @Test(.timeLimit(.minutes(20))) func theGuestIsGivenTheHostsTimeZone() async throws {
        guard Self.enabled else { return }
        let operations = try await readyOperations()
        let expected = try #require(GuestTimeZone.current())
        let given = try await operations.shell("getprop ro.boot.kernarium.timezone", limit: .seconds(10))
        #expect(given.text.trimmingCharacters(in: .whitespacesAndNewlines) == expected)
        guard ProcessInfo.processInfo.environment["ANDARIUM_TIME_ZONE_APPLIED"] == "1" else { return }
        var applied = ""
        let deadline = ContinuousClock.now + .seconds(90)
        while applied != expected, ContinuousClock.now < deadline {
            applied = try await operations.shell("getprop persist.sys.timezone", limit: .seconds(10))
                .text.trimmingCharacters(in: .whitespacesAndNewlines)
            if applied != expected { try await Task.sleep(for: .seconds(2)) }
        }
        print("ADBVSOCK: time zone given=\(expected) applied=\(applied)")
        #expect(applied == expected)
    }

    @MainActor
    @Test(.timeLimit(.minutes(20))) func aFreshGuestOpensTheSetupWizard() async throws {
        guard Self.enabled,
            ProcessInfo.processInfo.environment["ANDARIUM_OOBE_TESTS"] == "1"
        else { return }
        try #require(ProcessInfo.processInfo.environment["ANDARIUM_GUEST_INSTANCE"] == "recovery-display")
        let operations = try await readyOperations()
        let command = "getprop ro.setupwizard.mode; settings get global device_provisioned; settings get secure user_setup_complete; dumpsys activity activities | grep ResumedActivity"
        var state = try await operations.shell(command, limit: .seconds(10))
        // sys.boot_completed can precede the first resumed activity.
        let welcomeDeadline = ContinuousClock.now + .seconds(60)
        while !state.text.contains("org.lineageos.setupwizard/.WelcomeActivity"), ContinuousClock.now < welcomeDeadline {
            try await Task.sleep(for: .seconds(1))
            state = try await operations.shell(command, limit: .seconds(10))
        }
        print("ANDROID_OOBE: \(state.text)")
        #expect(state.text.hasPrefix("OPTIONAL\n0\n0\n"))
        #expect(state.text.contains("org.lineageos.setupwizard/.WelcomeActivity"))
        do {
            var tree = ""
            var lastError = "none"
            let uiDeadline = ContinuousClock.now + .seconds(60)
            // A user build restarts adbd when its trade-in initialization ends.
            // Reconnect after that transient loss before reading the visible UI.
            while !tree.contains("text=\"Start\""), ContinuousClock.now < uiDeadline {
                do {
                    tree = try await operations.shell(
                        "rm -f /data/local/tmp/andarium-setup.xml; uiautomator dump /data/local/tmp/andarium-setup.xml; cat /data/local/tmp/andarium-setup.xml; rm -f /data/local/tmp/andarium-setup.xml",
                        limit: .seconds(30)).text
                } catch { lastError = String(describing: error) }
                if !tree.contains("text=\"Start\"") { try await Task.sleep(for: .seconds(1)) }
            }
            print("ANDROID_OOBE_UI: \(tree)")
            #expect(tree.contains("text=\"Start\""), "Last UI query error: \(lastError)")
            #expect(tree.contains("package=\"org.lineageos.setupwizard\""))
            try await captureGuestFrame(named: "setup-welcome.png")
        } catch {
            Issue.record("Setup welcome screen could not be captured: \(error)")
        }

        let scene = try #require(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
        let model = try #require((scene.delegate as? AndariumSceneDelegate)?.model)
        // adbd can close the channel before replying to a successful shutdown.
        _ = try? await operations.shell("reboot -p", limit: .seconds(10))
        let deadline = ContinuousClock.now + .seconds(60)
        while model.phase != .home, ContinuousClock.now < deadline {
            try await Task.sleep(for: .milliseconds(200))
        }
        #expect(model.phase == .home && model.runEnd.poweredOff, "\(model.runReport)")
    }

    @Test(.timeLimit(.minutes(20)))
    func androidAdvertisesMultitouchToApplications() async throws {
        guard Self.enabled else { return }
        let operations = try await readyOperations()
        let result = try await operations.shell("pm list features")
        #expect(result.exitCode == 0)
        let features = Set(result.text.split(separator: "\n").map(String.init))
        for feature in [
            "android.hardware.touchscreen.multitouch",
            "android.hardware.touchscreen.multitouch.distinct",
            "android.hardware.touchscreen.multitouch.jazzhand",
        ] {
            #expect(features.contains("feature:\(feature)"))
        }
    }

    /// A standalone storage gate: the Android framework test APK, signed with
    /// the AOSP test key, is copied to tmp first. This opt-in run installs it,
    /// lets it call the one hidden API it uses (`hidden_api_policy` 1, put back
    /// afterwards), checks the public APIs, removes it and powers Android off.
    @Test(.timeLimit(.minutes(20)))
    func androidReportsExactStorageSizes() async throws {
        guard Self.enabled,
            ProcessInfo.processInfo.environment["ANDARIUM_STORAGE_TESTS"] == "1"
        else { return }
        let apk = FileManager.default.temporaryDirectory.appendingPathComponent("AndariumStorageTests.apk")
        defer { try? FileManager.default.removeItem(at: apk) }
        let bytes = try #require(try apk.resourceValues(forKeys: [.fileSizeKey]).fileSize)
        let operations = try await readyOperations()
        // The APK is test-key signed, so its one hidden API call needs the
        // policy; the guest's own value (unset: "null") is put back.
        let policy = try await operations.shell("settings get global hidden_api_policy").text
            .trimmingCharacters(in: .whitespacesAndNewlines)
        func restorePolicy() async {
            _ = try? await operations.shell(policy == "null" || policy.isEmpty
                ? "settings delete global hidden_api_policy"
                : "settings put global hidden_api_policy \(policy)")
        }
        do {
            try await operations.installApk(at: apk, sizeBytes: bytes) { _ in }
            let enabled = try await operations.shell("settings put global hidden_api_policy 1")
            #expect(enabled.exitCode == 0, "\(enabled.text)")
            let result = try await operations.shell(
                "am instrument -w -r -e class com.android.frameworks.andariumstorage.StorageAccountingTest#exactFilesystemBytes com.android.frameworks.andariumstorage.tests/androidx.test.runner.AndroidJUnitRunner",
                limit: .seconds(180))
            print("ANDROID_STORAGE: \(result.text)")
            #expect(result.exitCode == 0)
            #expect(result.text.contains("OK (1 test)"), "\(result.text)")
            #expect(result.text.contains("INSTRUMENTATION_CODE: -1"), "\(result.text)")
        } catch {
            await restorePolicy()
            _ = try? await operations.shell("pm uninstall com.android.frameworks.andariumstorage.tests")
            throw error
        }
        await restorePolicy()
        let removed = try await operations.shell("pm uninstall com.android.frameworks.andariumstorage.tests")
        #expect(removed.text.contains("Success"), "\(removed.text)")
        try await powerOffAndroid(operations)
    }

    /// Captures the real Settings UI, including categories below the fold.
    /// Images and accessibility hierarchies stay in tmp/StorageSettings for
    /// the runner to collect after this standalone opt-in test powers off.
    @Test(.timeLimit(.minutes(20)))
    func storageSettingsShowNonnegativeUsage() async throws {
        guard Self.enabled,
            ProcessInfo.processInfo.environment["ANDARIUM_STORAGE_TESTS"] == "1"
        else { return }
        let operations = try await readyOperations()
        _ = try await operations.shell("input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard")
        let launchCommand = "am start -W -n 'com.android.settings/.Settings$StorageDashboardActivity'"
        let launched = try await operations.shell(launchCommand)
        print("ANDROID_STORAGE_SETTINGS launch: \(launched.text)")
        #expect(launched.exitCode == 0, "\(launched.text)")
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("StorageSettings")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let dumpCommand = "uiautomator dump --compressed /data/local/tmp/andarium-storage-settings.xml >/dev/null && cat /data/local/tmp/andarium-storage-settings.xml"
        let deadline = ContinuousClock.now + .seconds(120)
        var hierarchy = ""
        repeat {
            try await Task.sleep(for: .seconds(3))
            let dump = try await operations.shell(dumpCommand)
            hierarchy = dump.text
            if hierarchy.contains("GB total") && !hierarchy.contains("Calculating") { break }
        } while ContinuousClock.now < deadline
        #expect(hierarchy.contains("GB total"), "\(hierarchy)")

        let size = try await operations.shell("wm size")
        let sizePattern = try NSRegularExpression(pattern: #"(\d+)x(\d+)"#)
        let sizeMatch = try #require(sizePattern.firstMatch(
            in: size.text, range: NSRange(size.text.startIndex..., in: size.text)))
        let width = try #require(Int((size.text as NSString).substring(with: sizeMatch.range(at: 1))))
        let height = try #require(Int((size.text as NSString).substring(with: sizeMatch.range(at: 2))))
        let negative = try NSRegularExpression(pattern: #"text="[−-][0-9]"#)
        let labels = try NSRegularExpression(pattern: #"text="([^"]+)"#)
        for page in 0..<3 {
            if page > 0 {
                _ = try await operations.shell(
                    "input swipe \(width / 2) \(height * 4 / 5) \(width / 2) \(height / 4) 500")
                try await Task.sleep(for: .seconds(2))
                hierarchy = try await operations.shell(dumpCommand).text
            }
            try Data(hierarchy.utf8).write(to: directory.appendingPathComponent("storage-\(page).xml"))
            let range = NSRange(hierarchy.startIndex..., in: hierarchy)
            let texts = labels.matches(in: hierarchy, range: range).map {
                (hierarchy as NSString).substring(with: $0.range(at: 1))
            }
            print("ANDROID_STORAGE_SETTINGS page=\(page): \(texts.joined(separator: " | "))")
            #expect(negative.firstMatch(in: hierarchy, range: range) == nil, "\(texts)")
            let screenshot = try await operations.shell("screencap -p")
            #expect(screenshot.exitCode == 0)
            #expect(screenshot.stdout.starts(with: [0x89, 0x50, 0x4E, 0x47]))
            try screenshot.stdout.write(to: directory.appendingPathComponent("storage-\(page).png"))
        }
        _ = try await operations.shell("rm /data/local/tmp/andarium-storage-settings.xml")
        try await powerOffAndroid(operations)
    }

    private func powerOffAndroid(_ operations: AdbGuestOperations) async throws {
        // The guest the test runs on: an isolated instance has its own record.
        let files = InstanceFiles(directory: AppEntry.guestDirectory(instance: AppEntry.fromArguments().overrides.instance))
        let before = try files.readState()?.lastCompleted?.at
        _ = try? await operations.shell("reboot -p", limit: .seconds(30))
        let deadline = ContinuousClock.now + .seconds(90)
        while ContinuousClock.now < deadline {
            if try files.readState()?.lastCompleted?.at != before { return }
            try await Task.sleep(for: .seconds(1))
        }
        Issue.record("Android did not finish its clean shutdown")
    }

    /// Waits until Android's input reader holds `count` contacts at the
    /// touchscreen, at the positions given, as `dumpsys input` reads them.
    private func awaitPointers(
        _ operations: AdbGuestOperations, _ count: Int, positions: [String]
    ) async throws {
        let deadline = ContinuousClock.now + .seconds(15)
        var state = ""
        while ContinuousClock.now < deadline {
            let dump = try await operations.shell("dumpsys input", limit: .seconds(15)).text
            if let name = dump.range(of: "Kernarium Touchscreen", options: .backwards) {
                state = String(dump[name.upperBound...])
                    .components(separatedBy: "\n  Device ")[0]
                if state.contains("Last Cooked Touch: pointerCount=\(count)") &&
                    positions.allSatisfy({ state.contains($0) }) {
                    return
                }
            }
            try await Task.sleep(for: .milliseconds(100))
        }
        Issue.record("Android did not read \(count) contacts: \(state)")
        throw CancellationError()
    }

    @Test(.timeLimit(.minutes(20)))
    func twoRuntimeContactsReachAndroidInputReader() async throws {
        guard Self.enabled else { return }
        let operations = try await readyOperations()
        _ = try await operations.shell("input keyevent KEYCODE_WAKEUP")
        defer {
            kernarium_runtime_loader_send_touch(0, 6000, 16000, 0)
            kernarium_runtime_loader_send_touch(1, 26000, 16000, 0)
        }

        kernarium_runtime_loader_send_touch(0, 10000, 16000, 1)
        kernarium_runtime_loader_send_touch(1, 22000, 16000, 1)
        try await awaitPointers(operations, 2, positions: ["x=10000, y=16000", "x=22000, y=16000"])
        kernarium_runtime_loader_send_touch(0, 6000, 16000, 1)
        kernarium_runtime_loader_send_touch(1, 26000, 16000, 1)
        try await awaitPointers(operations, 2, positions: ["x=6000, y=16000", "x=26000, y=16000"])
        kernarium_runtime_loader_send_touch(0, 6000, 16000, 0)
        try await awaitPointers(operations, 1, positions: ["x=26000, y=16000"])
        kernarium_runtime_loader_send_touch(1, 26000, 16000, 0)
        try await awaitPointers(operations, 0, positions: [])
    }

    /// What the guest's touchscreen node delivered, from a raw read of it.
    /// `getevent -l` buffers its output when it writes to a file or a pipe
    /// and loses it when stopped, so the node is read as a stream of
    /// `struct input_event` records instead: what `getevent` prints, as
    /// numbers.
    private struct EvdevEvent: Equatable {
        static let synchronize = EvdevEvent(type: 0, code: 0, value: 0)
        let type: UInt16
        let code: UInt16
        let value: Int32

        static func decode(_ data: Data) -> [EvdevEvent] {
            // 64-bit struct timeval, then type, code and value.
            let size = 24
            return stride(from: 0, to: data.count - size + 1, by: size).map { offset in
                let record = data.subdata(in: offset..<offset + size)
                return EvdevEvent(
                    type: record.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 16, as: UInt16.self) },
                    code: record.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 18, as: UInt16.self) },
                    value: record.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 20, as: Int32.self) })
            }
        }

        /// The events of each report, which ends at a SYN_REPORT.
        static func reports(_ events: [EvdevEvent]) -> [[EvdevEvent]] {
            var reports: [[EvdevEvent]] = []
            var current: [EvdevEvent] = []
            for event in events {
                if event == synchronize {
                    reports.append(current)
                    current = []
                } else {
                    current.append(event)
                }
            }
            return reports
        }

        static let touchButton = EvdevEvent(type: 1, code: 0x14a, value: 0)
        static func absolute(_ code: UInt16, _ value: Int32) -> EvdevEvent {
            EvdevEvent(type: 3, code: code, value: value)
        }
        static let toolType: UInt16 = 0x37
        static let trackingId: UInt16 = 0x39
        static let palm: Int32 = 2

        func isAbsolute(_ code: UInt16) -> Bool { type == 3 && self.code == code }
    }

    /// The touchscreen's event node as `getevent -p` lists it, with the
    /// listing of what it advertises.
    private func touchscreenNode(_ operations: AdbGuestOperations) async throws -> (node: String, listing: String) {
        let text = try await operations.shell("getevent -p -l", limit: .seconds(15)).text
        let device = try #require(
            text.components(separatedBy: "add device ").dropFirst()
                .first { $0.contains("\"Kernarium Touchscreen\"") },
            "getevent lists no Kernarium Touchscreen: \(text)")
        let line = try #require(device.split(separator: "\n").first)
        let path = try #require(line.split(separator: ":").last).trimmingCharacters(in: .whitespaces)
        try #require(path.hasPrefix("/dev/input/event"), "\(line)")
        return (path, device)
    }

    private static let evdevCapture = "/data/local/tmp/andarium-touch-cancel.evdev"

    /// Starts reading the node into a file in the guest, bounded to two
    /// minutes, and returns the reader's process.
    private func startReadingTouchscreen(_ operations: AdbGuestOperations, node: String) async throws -> Int {
        let started = try await operations.shell(
            "rm -f \(Self.evdevCapture); nohup timeout 120 cat \(node) > \(Self.evdevCapture) 2>/dev/null </dev/null & echo $!",
            limit: .seconds(15)).text.trimmingCharacters(in: .whitespacesAndNewlines)
        return try #require(Int(started), "no reader process: \(started)")
    }

    private func stopReadingTouchscreen(_ operations: AdbGuestOperations, process: Int) async throws -> [EvdevEvent] {
        let data = try await operations.shell("kill \(process); cat \(Self.evdevCapture); rm -f \(Self.evdevCapture)",
            limit: .seconds(15)).stdout
        return EvdevEvent.decode(data)
    }

    /// The lines InputReader logged since the last `logcat -c`, once one says
    /// a contact was cancelled for a palm (up to 15 seconds).
    private func inputReaderLog(_ operations: AdbGuestOperations) async throws -> String {
        let deadline = ContinuousClock.now + .seconds(15)
        var log = ""
        while ContinuousClock.now < deadline {
            log = try await operations.shell("logcat -d -s InputReader", limit: .seconds(15)).text
            if log.contains("for the palm event was detected") { break }
            try await Task.sleep(for: .milliseconds(250))
        }
        return log
    }

    /// A contact the runtime cancels reaches Android's input reader as a
    /// palm and then a lift: the guest's node reports MT_TOOL_PALM with the
    /// tracking ID kept and then the tracking ID ending (-1, `ffffffff` in
    /// `getevent`), InputReader logs the palm cancel, and the contact is gone
    /// from `dumpsys input` with no further touch.
    @Test(.timeLimit(.minutes(20)))
    func aCancelledContactReachesAndroidAsAPalmThenALift() async throws {
        guard Self.enabled else { return }
        let operations = try await readyOperations()
        _ = try await operations.shell("input keyevent KEYCODE_WAKEUP")
        defer {
            kernarium_runtime_loader_send_touch(0, 10000, 16000, 0)
            kernarium_runtime_loader_send_touch(1, 22000, 16000, 0)
        }
        let touchscreen = try await touchscreenNode(operations)
        // Android sees the tool type axis the host advertises: finger through palm.
        #expect(touchscreen.listing.contains("ABS_MT_TOOL_TYPE"), "\(touchscreen.listing)")
        let tool = try #require(touchscreen.listing.components(separatedBy: "\n")
            .first { $0.contains("ABS_MT_TOOL_TYPE") })
        #expect(tool.contains("min 0, max 2"), "\(tool)")

        _ = try await operations.shell("logcat -c")
        let reader = try await startReadingTouchscreen(operations, node: touchscreen.node)
        try await Task.sleep(for: .seconds(1))
        kernarium_runtime_loader_send_touch(0, 10000, 16000, 1)
        try await awaitPointers(operations, 1, positions: ["x=10000, y=16000"])
        kernarium_runtime_loader_cancel_touch(0)
        try await awaitPointers(operations, 0, positions: [])
        let events = try await stopReadingTouchscreen(operations, process: reader)
        let reports = EvdevEvent.reports(events)
        print("ANDROID_TOUCH_CANCEL: reports \(reports)")

        // The report that makes the contact a palm keeps its tracking ID; the
        // next one ends it and lifts BTN_TOUCH.
        let palm = try #require(reports.firstIndex { $0.contains(.absolute(EvdevEvent.toolType, EvdevEvent.palm)) },
            "no palm report: \(reports)")
        #expect(!reports[palm].contains { $0.isAbsolute(EvdevEvent.trackingId) }, "\(reports[palm])")
        #expect(reports.index(after: palm) < reports.count, "no report follows the palm: \(reports)")
        if reports.index(after: palm) < reports.count {
            let lift = reports[reports.index(after: palm)]
            #expect(lift.contains(.absolute(EvdevEvent.trackingId, -1)), "\(lift)")
            #expect(lift.contains(.touchButton), "\(lift)")
        }
        // The contact began before the palm, and nothing started another.
        #expect(reports[..<palm].contains {
            $0.contains { $0.isAbsolute(EvdevEvent.trackingId) && $0.value >= 0 }
        })
        #expect(!reports[reports.index(after: palm)...].contains {
            $0.contains { $0.isAbsolute(EvdevEvent.trackingId) && $0.value >= 0 }
        })

        let log = try await inputReaderLog(operations)
        print("ANDROID_TOUCH_CANCEL: InputReader \(log)")
        #expect(log.contains("Canceling pointer 0 for the palm event was detected"), "\(log)")
    }

    /// One finger of two cancelled leaves the other one down: Android's
    /// reader keeps one pointer, at the position it had.
    @Test(.timeLimit(.minutes(20)))
    func cancellingOneOfTwoContactsLeavesTheOtherDown() async throws {
        guard Self.enabled else { return }
        let operations = try await readyOperations()
        _ = try await operations.shell("input keyevent KEYCODE_WAKEUP")
        defer {
            kernarium_runtime_loader_send_touch(0, 10000, 16000, 0)
            kernarium_runtime_loader_send_touch(1, 22000, 16000, 0)
        }
        _ = try await operations.shell("logcat -c")
        kernarium_runtime_loader_send_touch(0, 10000, 16000, 1)
        kernarium_runtime_loader_send_touch(1, 22000, 16000, 1)
        try await awaitPointers(operations, 2, positions: ["x=10000, y=16000", "x=22000, y=16000"])
        kernarium_runtime_loader_cancel_touch(0)
        try await awaitPointers(operations, 1, positions: ["x=22000, y=16000"])
        let log = try await inputReaderLog(operations)
        #expect(log.contains("Canceling pointer 0 for the palm event was detected"), "\(log)")
        // The finger that was not cancelled moves and lifts as an ordinary one.
        kernarium_runtime_loader_send_touch(1, 24000, 16000, 1)
        try await awaitPointers(operations, 1, positions: ["x=24000, y=16000"])
        kernarium_runtime_loader_send_touch(1, 24000, 16000, 0)
        try await awaitPointers(operations, 0, positions: [])
        // A cancel of a contact that is not down changes nothing, and the
        // cancelled slot takes a finger again.
        kernarium_runtime_loader_cancel_touch(0)
        kernarium_runtime_loader_send_touch(0, 10000, 16000, 1)
        try await awaitPointers(operations, 1, positions: ["x=10000, y=16000"])
        kernarium_runtime_loader_send_touch(0, 10000, 16000, 0)
        try await awaitPointers(operations, 0, positions: [])
    }

    @Test(.timeLimit(.minutes(30))) func aBugReportIsAZipOfMoreThanAMegabyte() async throws {
        guard Self.enabled else { return }
        let operations = try await readyOperations()
        let started = ContinuousClock.now
        let progress = Progress()
        let file = try await operations.createBugReport { progress.add($0) }
        let seconds = (ContinuousClock.now - started).components.seconds
        print("ADBVSOCK: bug report made in \(seconds) s")
        defer { try? FileManager.default.removeItem(at: file) }
        let size = try #require(try file.resourceValues(forKeys: [.fileSizeKey]).fileSize)
        #expect(size > 1_000_000)
        #expect(try FileHandle(forReadingFrom: file).read(upToCount: 4) == Data([0x50, 0x4B, 0x03, 0x04]))
        #expect(file.deletingLastPathComponent() == BugReportStaging.directory)
        #expect(progress.values == progress.values.sorted())
        #expect((progress.values.last ?? 0) > 0.9)
    }

    @Test(.timeLimit(.minutes(20))) func cancellingABugReportStopsItAndLeavesNothing() async throws {
        guard Self.enabled else { return }
        let operations = try await readyOperations()
        let report = Task { try await operations.createBugReport { _ in } }
        try await Task.sleep(for: .seconds(15))
        report.cancel()
        let started = ContinuousClock.now
        do {
            _ = try await report.value
            Issue.record("the report finished before the cancel")
        } catch is CancellationError {
        }
        print("ADBVSOCK: bug report cancelled in \((ContinuousClock.now - started).components.seconds) s")
        // dumpstatez is stopped, and no staged file stays.
        var state = ""
        for _ in 0..<30 {
            state = try await operations.shell("getprop init.svc.dumpstatez").text.trimmingCharacters(in: .whitespacesAndNewlines)
            if state != "running" { break }
            try await Task.sleep(for: .seconds(1))
        }
        #expect(state != "running")
        let staged = (try? FileManager.default.contentsOfDirectory(atPath: BugReportStaging.directory.path)) ?? []
        #expect(staged.isEmpty)
    }

    @Test(.timeLimit(.minutes(20))) func usbDebuggingIsReadableThroughTheChannel() async throws {
        guard Self.enabled else { return }
        let operations = try await readyOperations()
        // Either answer is fine: what matters is that the channel works
        // whatever the setting is.
        let setting = await operations.usbDebugging()
        print("ADBVSOCK: USB debugging reads as \(setting.map(String.init(describing:)) ?? "unreadable")")
        #expect(setting != nil)
    }

    /// A computer on the network, here a loopback client of the bridge, reaches
    /// the guest's adbd through a fresh stream: adbd answers its CNXN with an
    /// AUTH challenge, since the computer's key is not the host's.
    @Test(.timeLimit(.minutes(20))) func theBridgeCarriesAComputersConnectionToAdbd() async throws {
        guard Self.enabled else { return }
        let operations = try await readyOperations()
        let bridge = AdbNetworkBridge(port: 0, opener: VsockConnector())
        let port = try bridge.start()
        defer { bridge.stop() }
        let descriptor = socket(AF_INET, SOCK_STREAM, 0)
        #expect(descriptor >= 0)
        defer { close(descriptor) }
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian
        address.sin_addr = in_addr(s_addr: UInt32(0x7F00_0001).bigEndian)
        let connected = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                connect(descriptor, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        #expect(connected == 0)
        var timeout = timeval(tv_sec: 20, tv_usec: 0)
        setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        let hello = AdbMessage(
            command: AdbWire.cnxn, arg0: AdbWire.version, arg1: UInt32(AdbWire.maxData),
            payload: Data("host::\0".utf8)
        ).encoded()
        #expect(hello.withUnsafeBytes { write(descriptor, $0.baseAddress, hello.count) } == hello.count)
        var header = [UInt8](repeating: 0, count: AdbWire.headerSize)
        var got = 0
        while got < header.count {
            let more = header.withUnsafeMutableBytes { read(descriptor, $0.baseAddress! + got, $0.count - got) }
            if more <= 0 { break }
            got += more
        }
        #expect(got == header.count)
        let reply = try AdbMessage.Header(Data(header))
        #expect(reply.command == AdbWire.auth || reply.command == AdbWire.cnxn)
        #expect(bridge.peerCount == 1)
    }

    /// A key the guest was not given is not authorized: adbd asks for a
    /// signature again, and the client reports that it was refused.
    @Test(.timeLimit(.minutes(20))) func aForeignKeyIsRefused() async throws {
        guard Self.enabled else { return }
        let operations = try await readyOperations()
        var error: Unmanaged<CFError>?
        let attributes: [String: Any] = [
            kSecAttrKeyType as String: kSecAttrKeyTypeRSA, kSecAttrKeySizeInBits as String: 2048,
        ]
        let foreign = try #require(SecKeyCreateRandomKey(attributes as CFDictionary, &error))
        let session = AdbSession(
            connector: VsockConnector(), signer: SecKeySigner(privateKey: foreign), clock: operations.clock)
        #expect(throws: AdbError.keyRejected) { try session.run { $0.banner } }
        // And the application's own key still works afterwards.
        let result = try await operations.shell("echo alive")
        #expect(result.text == "alive\n")
    }

    /// The channel is the application's own, whatever the guest's USB
    /// debugging setting is: the setting reads back through it, and it works
    /// while the setting is on and after both debugging switches are off
    /// (adbd restarts once then).
    @Test(.timeLimit(.minutes(20))) func theChannelWorksWhateverUSBDebuggingIsSet() async throws {
        guard Self.enabled else { return }
        let operations = try await readyOperations()
        let initial = await operations.usbDebugging()
        print("ADBVSOCK: USB debugging starts as \(initial.map(String.init(describing:)) ?? "unreadable")")
        func set(_ on: Bool) async throws {
            // The command's own connection can be cut by the restart.
            _ = try? await operations.shell(
                "settings put global adb_enabled \(on ? 1 : 0); settings put global adb_wifi_enabled 0")
            var read: Bool?
            for _ in 0..<60 {
                read = await operations.usbDebugging()
                if read == on { break }
                try await Task.sleep(for: .seconds(1))
            }
            #expect(read == on, "wanted \(on)")
            var alive = ""
            for _ in 0..<30 {
                if let result = try? await operations.shell("echo alive") {
                    alive = result.text
                    break
                }
                try await Task.sleep(for: .seconds(1))
            }
            #expect(alive == "alive\n", "after turning USB debugging \(on ? "on" : "off")")
        }
        try await set(true)
        try await set(false)
        try await set(initial ?? false)
    }

    /// The image's key rule, which only a guest booted with authorization on
    /// can show: the application's own key is accepted whatever the debugging
    /// setting; any other key is refused with no prompt and nothing queued
    /// while USB and wireless debugging are both off, and raises the stock
    /// "Allow USB debugging" prompt (left unanswered, so it is refused too)
    /// while debugging is on.
    @Test(.timeLimit(.minutes(25))) func theKeyRuleHoldsWithDebuggingOffAndOn() async throws {
        guard Self.enabled else { return }
        let operations = try await readyOperations()
        let own = try #require(operations.identity).privateKey
        let foreign = try Self.ephemeralKey()
        let initial = await operations.usbDebugging()

        let control = PromptControl(operations: operations)
        func setDebugging(_ on: Bool) async throws { try await control.setDebugging(on) }
        func promptCount() async throws -> Int { try await control.promptCount() }
        func dismissPrompt() async throws { try await control.dismissPrompt() }

        // Debugging off: the application's key connects, a foreign key does
        // not even when it offers itself, and no prompt is raised or queued.
        try await setDebugging(false)
        #expect(try await promptCount() == 0)
        #expect(try await Self.attempt(key: own, offer: false) == .accepted, "own key, debugging off")
        #expect(try await Self.attempt(key: foreign, offer: false) == .refused, "foreign key signing, debugging off")
        #expect(try await Self.attempt(key: foreign, offer: true) == .unauthorized, "foreign key offered, debugging off")
        #expect(try await promptCount() == 0, "a prompt was raised with debugging off")
        print("ADBVSOCK: key rule, debugging off: own accepted, foreign refused, no prompt")

        // Debugging on: nothing was queued meanwhile; the application's key
        // still connects; the foreign key raises the prompt and stays refused.
        try await setDebugging(true)
        try await Task.sleep(for: .seconds(5))
        #expect(try await promptCount() == 0, "a refused offer was queued for later")
        #expect(try await Self.attempt(key: own, offer: false) == .accepted, "own key, debugging on")
        #expect(try await Self.attempt(key: foreign, offer: false) == .refused, "foreign key signing, debugging on")
        #expect(try await Self.attempt(key: foreign, offer: true) == .unauthorized, "foreign key offered, debugging on")
        #expect(try await promptCount() > 0, "no prompt for an offered key with debugging on")
        print("ADBVSOCK: key rule, debugging on: own accepted, foreign refused, prompt raised")
        try await dismissPrompt()
        #expect(try await Self.attempt(key: own, offer: false) == .accepted, "own key after the prompt")

        try await setDebugging(initial ?? false)
    }

    /// The "Allow USB debugging" dialog can vanish without ever being
    /// answered: the process that shows it dies, or the system destroys its
    /// task before that process has created the activity, as it does while
    /// the guest is still busy starting up. Nothing then tells the framework,
    /// so stock Android leaves adbd's prompt pending and every later offer is
    /// skipped until adbd restarts. An image whose framework looks for the
    /// dialog 15 s after it starts it, and starts it again when it is gone,
    /// shows the dialog again by itself. This test kills the process that
    /// shows it (SystemUI) while the dialog is up, so it needs such an image:
    /// `ANDARIUM_ADB_REARM_TESTS=1`.
    @Test(.timeLimit(.minutes(25))) func aPromptWhoseDialogDiedIsRaisedAgain() async throws {
        guard Self.enabled, ProcessInfo.processInfo.environment["ANDARIUM_ADB_REARM_TESTS"] == "1" else { return }
        let operations = try await readyOperations()
        let foreign = try Self.ephemeralKey()
        let initial = await operations.usbDebugging()
        let control = PromptControl(operations: operations)

        try await control.setDebugging(true)
        try await Task.sleep(for: .seconds(5))
        #expect(try await Self.attempt(key: foreign, offer: true) == .unauthorized, "foreign key offered")
        #expect(try await control.awaitPrompt(seconds: 90), "no prompt for the first offer")

        // The dialog dies with the process that shows it, with no answer to adbd. This must happen
        // within the 15 s the framework gives the dialog before it looks for it once.
        _ = try? await operations.shell("am crash com.android.systemui")
        var gone = false
        for _ in 0..<60 {
            if try await control.promptCount() == 0 {
                gone = true
                break
            }
            try await Task.sleep(for: .seconds(1))
        }
        #expect(gone, "the dialog did not go away with SystemUI")

        // No new offer: adbd ignores requests while its prompt is dispatched. The framework starts
        // the dialog again by itself when it finds it gone.
        let raised = try await control.awaitPrompt(seconds: 90)
        #expect(raised, "the dialog was not raised again: adbd's prompt is still pending for the dead one")
        print("ADBVSOCK: prompt raised again after its dialog died: \(raised)")
        try await control.dismissPrompt()
        try await control.setDebugging(initial ?? false)
    }

    /// The framework's single look at the dialog, 15 s after it started it, must
    /// find a dialog that is up and leave it alone: a second dialog for the
    /// same prompt would stack on the first. The restarts of a missed dialog
    /// come 15 s apart, so 50 s show whether one was made.
    @Test(.timeLimit(.minutes(20))) func aPromptWhoseDialogIsUpIsNotRaisedTwice() async throws {
        guard Self.enabled, ProcessInfo.processInfo.environment["ANDARIUM_ADB_REARM_TESTS"] == "1" else { return }
        let operations = try await readyOperations()
        let foreign = try Self.ephemeralKey()
        let initial = await operations.usbDebugging()
        let control = PromptControl(operations: operations)

        try await control.setDebugging(true)
        try await Task.sleep(for: .seconds(5))
        #expect(try await Self.attempt(key: foreign, offer: true) == .unauthorized, "foreign key offered")
        #expect(try await control.awaitPrompt(seconds: 90), "no prompt for the offer")
        let before = try await control.distinctDialogs()
        #expect(before == 1, "one dialog expected while the prompt is awaited, found \(before)")
        try await Task.sleep(for: .seconds(50))
        let after = try await control.distinctDialogs()
        #expect(after == 1, "the dialog that stayed up was raised again: \(after) dialogs")
        print("ADBVSOCK: dialogs before and after the look: \(before) \(after)")
        try await control.dismissPrompt()
        try await control.setDebugging(initial ?? false)
    }

    /// A real adb on the Mac against the bridge, for as long as the launch
    /// asks (`ANDARIUM_ADB_NET_HOLD_SECONDS`, or until the file named
    /// `adb-net-done` appears in the printed folder). The application's own
    /// key is written there as `adbkey` for the Mac's adb to offer, since the
    /// guest authorizes that key for the boot.
    @Test(.timeLimit(.minutes(60))) func holdTheBridgeForARealAdbClient() async throws {
        guard Self.enabled,
            let text = ProcessInfo.processInfo.environment["ANDARIUM_ADB_NET_HOLD_SECONDS"],
            let seconds = Double(text), seconds > 0
        else { return }
        let operations = try await readyOperations()
        let identity = try #require(operations.identity)
        var error: Unmanaged<CFError>?
        let der = try #require(SecKeyCopyExternalRepresentation(identity.privateKey, &error) as Data?)
        let body = der.base64EncodedString(options: [.lineLength64Characters, .endLineWithLineFeed])
        let directory = FileManager.default.temporaryDirectory
        try "-----BEGIN RSA PRIVATE KEY-----\n\(body)\n-----END RSA PRIVATE KEY-----\n"
            .write(to: directory.appendingPathComponent("adbkey"), atomically: true, encoding: .utf8)
        let done = directory.appendingPathComponent("adb-net-done")
        try? FileManager.default.removeItem(at: done)
        let bridge = AdbNetworkBridge(opener: VsockConnector())
        let port = try bridge.start()
        defer { bridge.stop() }
        print("ADBVSOCK: bridge ready port=\(port) address=\(LanAddress.current() ?? "none") folder=\(directory.path)")
        let deadline = ContinuousClock.now + .seconds(seconds)
        var peak = 0
        while ContinuousClock.now < deadline, !FileManager.default.fileExists(atPath: done.path) {
            peak = max(peak, bridge.peerCount)
            try await Task.sleep(for: .milliseconds(500))
        }
        print("ADBVSOCK: bridge served at most \(peak) computers at once")
    }

    /// The key the application made for this boot changes when the guest is
    /// erased. A first launch records its line in the file the run names; a
    /// launch that erased the guest first (ANDARIUM_ERASE=1) must differ.
    @Test(.timeLimit(.minutes(20))) func eraseGuestMakesANewKey() async throws {
        guard Self.enabled, let path = ProcessInfo.processInfo.environment["ANDARIUM_ADB_KEY_FILE"] else { return }
        // The key adbd accepts: the one this boot was given, not one an erase
        // that is still running is about to delete.
        let line = try #require(try await readyOperations().identity).publicKeyLine
        let file = URL(fileURLWithPath: path)
        if let previous = try? String(contentsOf: file, encoding: .utf8) {
            #expect(previous != line)
        }
        try line.write(to: file, atomically: true, encoding: .utf8)
    }

    @Test(.timeLimit(.minutes(20))) func aSystemApkPulledFromTheImageInstallsAndTheUninstallRestoresIt() async throws {
        guard Self.enabled else { return }
        let operations = try await readyOperations()
        // The smallest base APKs of system apps that are not privileged.
        let listing = try await operations.shell("pm list packages -s -f").text
        let apps = listing.split(separator: "\n").compactMap { line -> (path: String, package: String)? in
            guard line.hasPrefix("package:"), let equals = line.lastIndex(of: "=") else { return nil }
            let path = String(line[line.index(line.startIndex, offsetBy: 8)..<equals])
            guard path.hasSuffix(".apk"), path.contains("/app/"), !path.contains("/priv-app/") else { return nil }
            return (path, String(line[line.index(after: equals)...]))
        }
        #expect(!apps.isEmpty)
        let quoted = apps.map { "'\($0.path)'" }.joined(separator: " ")
        let sizes = try await operations.shell("stat -c '%s %n' \(quoted)").text
        let ordered = sizes.split(separator: "\n").compactMap { line -> (size: Int, path: String)? in
            let parts = line.split(separator: " ", maxSplits: 1)
            guard parts.count == 2, let size = Int(parts[0]), size > 50_000 else { return nil }
            return (size, String(parts[1]))
        }.sorted { $0.size < $1.size }
        var outcomes: [String] = []
        for candidate in ordered.prefix(5) {
            let package = try #require(apps.first { $0.path == candidate.path }).package
            let local = FileManager.default.temporaryDirectory.appendingPathComponent("pulled-\(UUID().uuidString).apk")
            defer { try? FileManager.default.removeItem(at: local) }
            try await pull(candidate.path, to: local, operations: operations)
            do {
                let started = ContinuousClock.now
                try await operations.installApk(at: local, sizeBytes: candidate.size) { _ in }
                let elapsed = ContinuousClock.now - started
                let seconds = Double(elapsed.components.seconds) + Double(elapsed.components.attoseconds) / 1e18
                print("ADBVSOCK: install of \(candidate.size) bytes took \(String(format: "%.1f", seconds)) s")
            } catch {
                outcomes.append("\(package): \(error.localizedDescription)")
                continue
            }
            // The update is in /data/app; removing it restores the system's copy.
            let path = try await operations.shell("pm path \(package)").text
            #expect(path.contains("/data/app/"), "\(path)")
            let removal = try await operations.shell("pm uninstall \(package)")
            #expect(removal.text.contains("Success"), "\(removal.text)")
            let restored = try await operations.shell("pm path \(package)").text
            #expect(restored.contains(candidate.path), "\(restored)")
            return
        }
        Issue.record("no system APK took an update: \(outcomes)")
    }

    private func pull(_ remote: String, to local: URL, operations: AdbGuestOperations) async throws {
        FileManager.default.createFile(atPath: local.path, contents: nil)
        let handle = try FileHandle(forWritingTo: local)
        defer { try? handle.close() }
        let sink = HandleSink(handle)
        let identity = try #require(operations.identity)
        try await AdbOperationRunner.run(identity: identity, clock: operations.clock) { session in
            try session.run { connection in
                session.watchdog.arm(idle: .seconds(60), overall: .seconds(600))
                let sync = try AdbSync(connection: connection)
                defer { sync.quit() }
                try sync.receive(remote) { try sink.write($0) }
            }
        }
    }
}

/// The guest's debugging setting and its "Allow USB debugging" prompt, as the
/// debugging tests drive them.
fileprivate struct PromptControl {
    let operations: AdbGuestOperations

    func setDebugging(_ on: Bool) async throws {
        _ = try? await operations.shell(
            "settings put global adb_enabled \(on ? 1 : 0); settings put global adb_wifi_enabled 0")
        var read: Bool?
        for _ in 0..<60 {
            read = await operations.usbDebugging()
            if read == on { break }
            try await Task.sleep(for: .seconds(1))
        }
        #expect(read == on)
        // adbd restarts once when both switches go off.
        try await Task.sleep(for: .seconds(3))
    }

    /// The number of lines naming the dialog's activity in the guest's activity dump.
    func promptCount() async throws -> Int {
        var text = ""
        for _ in 0..<20 {
            if let result = try? await operations.shell(
                "dumpsys activity activities | grep -c UsbDebuggingActivity")
            {
                text = result.text
                break
            }
            try await Task.sleep(for: .seconds(1))
        }
        return Int(text.trimmingCharacters(in: .whitespacesAndNewlines)) ?? -1
    }

    /// The number of distinct activity records of the dialog in the guest's
    /// activity dump: a dialog raised again is a new record.
    func distinctDialogs() async throws -> Int {
        var text = ""
        for _ in 0..<20 {
            if let result = try? await operations.shell(
                "dumpsys activity activities | grep -o 'ActivityRecord{[0-9a-f]* u[0-9]* [^ ]*UsbDebuggingActivity' | sort -u | wc -l")
            {
                text = result.text
                break
            }
            try await Task.sleep(for: .seconds(1))
        }
        return Int(text.trimmingCharacters(in: .whitespacesAndNewlines)) ?? -1
    }

    func dismissPrompt() async throws {
        for _ in 0..<10 {
            _ = try? await operations.shell("input keyevent KEYCODE_BACK")
            try await Task.sleep(for: .seconds(1))
            if try await promptCount() == 0 { return }
        }
        Issue.record("the debugging prompt did not go away")
    }

    /// Waits for the dialog's activity to exist.
    func awaitPrompt(seconds: Int) async throws -> Bool {
        for _ in 0..<seconds {
            if try await promptCount() > 0 { return true }
            try await Task.sleep(for: .seconds(1))
        }
        return false
    }
}

extension AdbOverVsockTests {
    fileprivate enum KeyOutcome: Equatable {
        /// adbd answered with its CONNECT: the key is authorized.
        case accepted
        /// adbd asked for a signature again after this key's.
        case refused
        /// The key was offered (AUTH RSAPUBLICKEY) and adbd did not accept it.
        case unauthorized
    }

    fileprivate static func ephemeralKey() throws -> SecKey {
        var error: Unmanaged<CFError>?
        let attributes: [String: Any] = [
            kSecAttrKeyType as String: kSecAttrKeyTypeRSA, kSecAttrKeySizeInBits as String: 2048,
        ]
        return try #require(SecKeyCreateRandomKey(attributes as CFDictionary, &error))
    }

    /// A connection that signs adbd's token with `key` and, when `offer` is
    /// set, offers the key's public half after adbd asks again, as a computer
    /// that wants the person to allow it does.
    fileprivate static func attempt(key: SecKey, offer: Bool) async throws -> KeyOutcome {
        // The waits block, so they run off the main thread.
        try await Task.detached { try attemptBlocking(key: key, offer: offer) }.value
    }

    private nonisolated static func attemptBlocking(key: SecKey, offer: Bool) throws -> KeyOutcome {
        let transport = try VsockConnector().connect()
        defer { transport.shutdown() }
        let connection = AdbConnection(transport: transport)
        var hello = Data(AdbWire.hostBanner.utf8)
        hello.append(0)
        try connection.send(AdbMessage(
            command: AdbWire.cnxn, arg0: AdbWire.version, arg1: UInt32(AdbWire.maxData), payload: hello))
        let signer = SecKeySigner(privateKey: key)
        var stage = 0
        while true {
            let message: AdbMessage?
            do {
                message = try receive(connection, transport: transport, seconds: stage == 2 ? 6 : 20)
            } catch {
                // adbd ended the connection.
                return stage == 2 ? .unauthorized : .refused
            }
            guard let message else { return stage == 2 ? .unauthorized : .refused }
            switch message.command {
            case AdbWire.cnxn:
                return .accepted
            case AdbWire.auth where message.arg0 == AdbWire.authToken:
                switch stage {
                case 0:
                    try connection.send(AdbMessage(
                        command: AdbWire.auth, arg0: AdbWire.authSignature,
                        payload: signer.sign(token: message.payload)))
                    stage = 1
                case 1 where offer:
                    var line = Data(try AdbKeyStore.line(for: key).utf8)
                    line.append(contentsOf: Array(" andarium-test@guest".utf8))
                    line.append(0)
                    try connection.send(AdbMessage(
                        command: AdbWire.auth, arg0: AdbWire.authRSAPublicKey, payload: line))
                    stage = 2
                case 1:
                    return .refused
                default:
                    return .unauthorized
                }
            default:
                throw AdbError.protocolViolation("a message of \(message.command) in the handshake")
            }
        }
    }

    /// One message, or nil when none came within `seconds` (the connection is
    /// then ended).
    fileprivate nonisolated static func receive(
        _ connection: AdbConnection, transport: any AdbTransport, seconds: Double
    ) throws -> AdbMessage? {
        let box = ReceiveBox()
        let done = DispatchSemaphore(value: 0)
        let thread = Thread {
            box.set(Result { try connection.receive() })
            done.signal()
        }
        thread.start()
        if done.wait(timeout: .now() + seconds) == .timedOut {
            transport.shutdown()
            _ = done.wait(timeout: .now() + 5)
            return nil
        }
        return try box.get()
    }
}

private nonisolated final class ReceiveBox: @unchecked Sendable {
    private let lock = NSLock()
    private var result: Result<AdbMessage, Error>?
    func set(_ result: Result<AdbMessage, Error>) { lock.withLock { self.result = result } }
    func get() throws -> AdbMessage? {
        try lock.withLock { try result?.get() }
    }
}

/// Collects a bug report's progress from the operation's thread.
private nonisolated final class Progress: @unchecked Sendable {
    private let lock = NSLock()
    private var all: [Double] = []

    var values: [Double] { lock.withLock { all } }

    func add(_ update: BusyUpdate) {
        guard let fraction = update.progress else { return }
        lock.withLock { all.append(fraction) }
    }
}

private nonisolated final class HandleSink: @unchecked Sendable {
    private let handle: FileHandle

    init(_ handle: FileHandle) {
        self.handle = handle
    }

    func write(_ data: Data) throws {
        try handle.write(contentsOf: data)
    }
}
