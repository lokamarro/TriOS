// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// Create bug report and Install App as the model plays them: Settings has
/// dismissed, the overlay covers the screens, a Cancel stops the work, and the
/// result is a share sheet, a row or an alert. A stub stands for the runtime.
/// The tests share the staging folder, which one of them sweeps, so they run
/// one at a time.
///
/// The motion toggle persists and reaches the runtime driver
/// (`motionSensorsAreOnUntilChosenOffAndTheChoiceIsPersisted`).
@Suite(.serialized)
@MainActor
struct AndroidOperationTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func running(_ mode: StartMode = .normal) -> (AppModel, StubDriver) {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1
        )
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.phase = .running(mode)
        return (model, driver)
    }

    /// Waits, up to a bound, for `condition` on the main actor.
    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return condition()
    }

    private func stagedFile() throws -> URL {
        try FileManager.default.createDirectory(at: BugReportStaging.directory, withIntermediateDirectories: true)
        let url = BugReportStaging.directory.appendingPathComponent("test-\(UUID().uuidString).zip")
        try Data([0x50, 0x4B, 5, 6]).write(to: url)
        return url
    }

    /// A gate the stub waits on until the test opens it.
    private nonisolated final class Gate: @unchecked Sendable {
        private let lock = NSLock()
        private var open = false
        func release() { lock.withLock { open = true } }
        var isOpen: Bool { lock.withLock { open } }
    }

    @Test func motionSensorsAreOnUntilChosenOffAndTheChoiceIsPersisted() {
        let (model, driver) = running()
        // No choice yet: on where the device has sensors, and nothing stored.
        #expect(model.guest?.motionChoice == nil)
        #expect(model.motionSensorsOn)
        model.setMotionSensors(false)
        #expect(model.guest?.motionChoice == false)
        #expect(driver.savedGuest?.motionChoice == false)
        #expect(!model.motionSensorsOn)
        #expect(!driver.motionSensors)
        model.setMotionSensors(true)
        #expect(model.guest?.motionChoice == true)
        #expect(driver.savedGuest?.motionChoice == true)
        #expect(model.motionSensorsOn)
        #expect(driver.motionSensors)
        // Where the device has no sensors the switch shows off, whatever was chosen.
        model.motionSensorsAvailable = false
        #expect(!model.motionSensorsOn)
        model.setMotionSensors(true)
        #expect(model.guest?.motionChoice == true && !model.motionSensorsOn)
    }

    // MARK: Bug report

    @Test func aBugReportCoversTheScreensThenOffersTheFile() async throws {
        let (model, driver) = running()
        let file = try stagedFile()
        driver.bugReport = { update in
            update(BusyUpdate(progress: 0.45))
            update(BusyUpdate(progress: 0.3))  // never moves back
            update(BusyUpdate(progress: 0.9, detail: "Copying the report\u{2026}"))
            return file
        }
        var offered: URL?
        var finishedSharing: (@MainActor () -> Void)?
        model.presentShare = { url, done in
            offered = url
            finishedSharing = done
            return true
        }
        model.createBugReport()
        #expect(model.busy?.title == "Creating bug report")
        #expect(model.busy?.cancel != nil)
        #expect(await eventually { offered != nil })
        #expect(offered == file)
        #expect(model.busy == nil && model.alert == nil)
        // The file stays for the sheet, and goes on any outcome of it.
        #expect(FileManager.default.fileExists(atPath: file.path))
        finishedSharing?()
        #expect(!FileManager.default.fileExists(atPath: file.path))
    }

    @Test func progressOnlyMovesForward() async throws {
        let (model, driver) = running()
        let gate = Gate()
        driver.bugReport = { update in
            update(BusyUpdate(progress: 0.5))
            update(BusyUpdate(progress: 0.2))
            while !gate.isOpen { try await Task.sleep(for: .milliseconds(5)) }
            throw CancellationError()
        }
        model.createBugReport()
        #expect(await eventually { model.busy?.progress == 0.5 })
        gate.release()
        #expect(await eventually { model.busy == nil })
    }

    @Test func cancellingStopsTheReportAndReturnsToTheSession() async throws {
        let (model, driver) = running()
        let stopped = Gate()
        driver.bugReport = { _ in
            do {
                while true { try await Task.sleep(for: .milliseconds(5)) }
            } catch {
                stopped.release()
                throw error
            }
        }
        model.createBugReport()
        let cancel = try #require(model.busy?.cancel)
        cancel.action()
        // The overlay stays while the guest is cleaned up, with no way to cancel again.
        #expect(model.busy?.cancel == nil)
        #expect(model.busy?.detail == "Cancelling\u{2026}")
        #expect(await eventually { model.busy == nil })
        #expect(stopped.isOpen)
        #expect(model.alert == nil)
    }

    @Test func aFailureIsAnAlertAndTheOverlayGoes() async throws {
        let (model, driver) = running()
        driver.bugReport = { _ in throw AdbFailure(.remote("There is an existing bugreport in progress"), activity: .bugReport) }
        model.createBugReport()
        #expect(await eventually { model.alert != nil })
        #expect(model.alert?.title == "Bug report not created")
        #expect(model.alert?.message == "The guest is already making a bug report.")
        #expect(model.busy == nil)
    }

    @Test func recoveryOffersNoBugReport() {
        let (model, _) = running(.recovery)
        model.createBugReport()
        #expect(model.busy == nil)
    }

    @Test func aSecondOperationWaitsForTheFirst() async throws {
        let (model, driver) = running()
        let gate = Gate()
        driver.bugReport = { _ in
            while !gate.isOpen { try await Task.sleep(for: .milliseconds(5)) }
            throw CancellationError()
        }
        model.createBugReport()
        let first = model.busy
        model.createBugReport()
        #expect(model.busy == first)
        gate.release()
        #expect(await eventually { model.busy == nil })
    }

    @Test func theEndOfTheRunTakesTheOperationWithoutAnAlert() async throws {
        let (model, driver) = running()
        let gate = Gate()
        driver.bugReport = { _ in
            while !gate.isOpen { try await Task.sleep(for: .milliseconds(5)) }
            throw AdbFailure(.guestGone, activity: .bugReport)
        }
        model.createBugReport()
        #expect(model.busy != nil)
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 1)
        #expect(model.busy == nil)
        gate.release()
        try await Task.sleep(for: .milliseconds(100))
        #expect(model.alert == nil && model.busy == nil)
    }

    @Test func aDumpThatMayGoOnIsStoppedAfterAnyEndOfTheStreamButNotAfterALocalFailure() {
        #expect(BugReportOperation.dumpMayGoOn(after: CancellationError()))
        for error in [AdbError.cancelled, .timedOut, .guestGone, .streamClosed] {
            #expect(BugReportOperation.dumpMayGoOn(after: error), "\(error)")
        }
        for error in [AdbError.refused, .notAnswering, .keyRejected, .io(ENOSPC), .remote("no")] {
            #expect(!BugReportOperation.dumpMayGoOn(after: error), "\(error)")
        }
        #expect(!BugReportOperation.dumpMayGoOn(after: BugReportStaging.NoRoom(bytes: 1 << 20, device: "iPad")))
    }

    @Test func theFailuresHaveTheirWords() {
        #expect(AdbFailure(.refused, activity: .bugReport).message
            == "The guest isn\u{2019}t ready to make a bug report. Try again in a moment.")
        #expect(AdbFailure(.notAnswering, activity: .installApp).message
            == "The guest isn\u{2019}t answering yet. Try again once it has started.")
        #expect(AdbFailure(.keyRejected, activity: .bugReport).message
            == "The guest didn\u{2019}t accept Andarium\u{2019}s key. Restart the guest and try again.")
        #expect(AdbFailure(.guestGone, activity: .installApp).message
            == "The guest\u{2019}s debugging service restarted before the app was installed. Try again.")
        #expect(AdbFailure(.streamClosed, activity: .bugReport).message
            == "The guest\u{2019}s debugging service restarted before the bug report was finished. Try again.")
        #expect(BugReportStaging.NoRoom(bytes: 300 << 20, device: "iPad").errorDescription
            == "There isn\u{2019}t enough free space on this iPad for the report (300 MiB).")
    }

    // MARK: Staging

    @Test func theStagedReportNeedsRoomBeyondItsOwnSize() throws {
        #expect(throws: BugReportStaging.NoRoom(bytes: 2_000_000_000, device: "iPad")) {
            try BugReportStaging.makeSink(
                remotePath: "/data/b/bugreport-x.zip", size: 2_000_000_000, device: "iPad",
                available: 2_000_000_000 + BugReportStaging.reserveBytes - 1)
        }
        let sink = try BugReportStaging.makeSink(
            remotePath: "/data/b/bugreport-x.zip", size: 10, device: "iPad", available: 1 << 30)
        defer { BugReportStaging.sweep() }
        try sink.write(Data("PK".utf8))
        sink.close()
        #expect(sink.url.deletingLastPathComponent() == BugReportStaging.directory)
        #expect(sink.url.lastPathComponent == "bugreport-x.zip")
        BugReportStaging.sweep()
        #expect(!FileManager.default.fileExists(atPath: sink.url.path))
    }

    // MARK: Install

    private func apk(named name: String) throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(UUID().uuidString)-\(name)")
        try Data(count: 3_000_000).write(to: url)
        return url
    }

    @Test func anApkInstallsBehindTheOverlayAndTheResultStaysUntilItIsClosed() async throws {
        let (model, driver) = running()
        driver.install = { update in
            update(BusyUpdate(progress: 0.4))
            update(BusyUpdate(progress: 0.8, detail: "The guest is installing the app\u{2026}", cancellable: false))
        }
        let url = try apk(named: "Signal-7.21.4.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(model.busy?.title == "Installing app")
        #expect(model.busy?.result == nil)
        #expect(await eventually { model.busy?.result != nil })
        // The overlay is still up, and says how it went.
        let busy = try #require(model.busy)
        #expect(busy.title == "App installed")
        #expect(busy.detail.hasSuffix("Signal-7.21.4.apk is installed in the guest."))
        #expect(busy.result?.failed == false)
        #expect(busy.progress == nil && busy.cancel == nil)
        #expect(model.alert == nil)
        // Nothing else may start while it is up.
        model.createBugReport()
        #expect(model.busy?.title == "App installed")
        busy.result?.dismiss.action()
        #expect(model.busy == nil)
    }

    @Test func theCancelButtonGoesOnceTheLastByteIsSent() async throws {
        let (model, driver) = running()
        let gate = Gate()
        driver.install = { update in
            update(BusyUpdate(progress: 0.8, detail: "The guest is installing the app\u{2026}", cancellable: false))
            while !gate.isOpen { try await Task.sleep(for: .milliseconds(5)) }
        }
        let url = try apk(named: "a.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(model.busy?.cancel != nil)
        #expect(await eventually { model.busy?.cancel == nil })
        #expect(model.busy?.detail == "The guest is installing the app\u{2026}")
        gate.release()
        #expect(await eventually { model.busy?.result != nil })
    }

    @Test func cancellingTheInstallLeavesNoResultAndNoAlert() async throws {
        let (model, driver) = running()
        driver.install = { _ in
            while true { try await Task.sleep(for: .milliseconds(5)) }
        }
        let url = try apk(named: "a.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        model.busy?.cancel?.action()
        #expect(await eventually { model.busy == nil })
        #expect(model.alert == nil)
    }

    @Test func aRefusalByTheGuestIsTheResultOnTheOverlayNotAnAlert() async throws {
        let (model, driver) = running()
        driver.install = { _ in
            throw AdbFailure(message: PackageInstallMessages.message(forCode: "FAILED_VERSION_DOWNGRADE"))
        }
        let url = try apk(named: "a.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.busy?.result != nil })
        #expect(model.busy?.title == "App not installed")
        #expect(model.busy?.detail == "A newer version of this app is already installed.")
        #expect(model.busy?.result?.failed == true)
        #expect(model.alert == nil)
        model.busy?.result?.dismiss.action()
        #expect(model.busy == nil)
    }

    @Test func anythingButASingleApkIsRefusedAtOnceOnTheSameOverlay() throws {
        let (model, driver) = running()
        let expected: [(String, InstallRefusal)] = [
            ("Trip.zip", .notApk), ("app.apks", .bundle), ("app.xapk", .bundle), ("app.apkm", .bundle),
            ("notes.txt", .notApk),
        ]
        for (name, refusal) in expected {
            let url = try apk(named: name)
            defer { try? FileManager.default.removeItem(at: url) }
            model.installPicked(url)
            let busy = try #require(model.busy, "\(name)")
            #expect(busy.title == "App not installed", "\(name)")
            #expect(busy.detail == refusal.message, "\(name)")
            #expect(busy.result?.failed == true && busy.cancel == nil && busy.progress == nil, "\(name)")
            #expect(model.alert == nil)
            busy.result?.dismiss.action()
            #expect(model.busy == nil)
        }
        // Nothing was sent to the guest.
        #expect(driver.installed == 0)
    }

    @Test func theEndOfTheRunTakesAFinishedResultAway() async throws {
        let (model, _) = running()
        let url = try apk(named: "a.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.busy?.result != nil })
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
        #expect(model.busy == nil)
    }
}

/// ADB over network as the model plays it: the toggle, the guest's USB
/// debugging setting, the address, and the end of a run.
@MainActor
struct AdbOverNetworkModelTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func running(_ mode: StartMode = .normal) -> (AppModel, StubDriver) {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1
        )
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.phase = .running(mode)
        model.adbWatchInterval = .milliseconds(20)
        return (model, driver)
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return condition()
    }

    @Test func theToggleWaitsForTheGuestsUsbDebuggingToBeKnownOn() async {
        let (model, driver) = running()
        #expect(!model.adbOverNetworkAvailable)
        model.setAdbOverNetwork(true)
        #expect(!model.session.adb && driver.starts == 0)
        driver.debugging = false
        await model.refreshUsbDebugging()
        #expect(model.session.usbDebugging == false && !model.adbOverNetworkAvailable)
        model.setAdbOverNetwork(true)
        #expect(!model.session.adb && driver.starts == 0)
        driver.debugging = true
        await model.refreshUsbDebugging()
        #expect(model.adbOverNetworkAvailable)
        model.setAdbOverNetwork(true)
        #expect(model.session.adb && driver.listening && driver.starts == 1)
        #expect(model.session.adbAddress == "192.0.2.23")
        // Again changes nothing.
        model.setAdbOverNetwork(true)
        #expect(driver.starts == 1)
        model.setAdbOverNetwork(false)
        #expect(!model.session.adb && !driver.listening && model.session.adbAddress == nil)
    }

    @Test func usbDebuggingGoingOffSwitchesItOffAndClosesTheComputersConnected() async {
        let (model, driver) = running()
        await model.refreshUsbDebugging()
        model.setAdbOverNetwork(true)
        #expect(model.session.adb)
        driver.connectedComputers = 2
        driver.debugging = nil
        await model.refreshUsbDebugging()
        #expect(model.session.adb && driver.listening, "a setting that cannot be read changes nothing")
        #expect(driver.closedComputers == 0)
        driver.debugging = false
        await model.refreshUsbDebugging()
        #expect(!model.session.adb && !driver.listening)
        // The listener goes and so does every connection already made.
        #expect(driver.closedComputers == 2 && driver.connectedComputers == 0)
        #expect(!model.adbOverNetworkAvailable)
    }

    @Test func theWatcherNoticesUsbDebuggingGoingOffWithSettingsClosed() async {
        let (model, driver) = running()
        await model.refreshUsbDebugging()
        model.setAdbOverNetwork(true)
        driver.connectedComputers = 1
        driver.debugging = false
        #expect(await eventually { !model.session.adb && !driver.listening })
        #expect(driver.closedComputers == 1)
    }

    @Test func theAddressFollowsTheNetworkAndMayBeMissing() async {
        let (model, driver) = running()
        await model.refreshUsbDebugging()
        driver.address = nil
        model.setAdbOverNetwork(true)
        #expect(model.session.adb && model.session.adbAddress == nil)
        driver.address = "198.51.100.8"
        #expect(await eventually { model.session.adbAddress == "198.51.100.8" })
        driver.address = nil
        #expect(await eventually { model.session.adbAddress == nil })
        model.setAdbOverNetwork(false)
    }

    @Test func aPortInUseIsSaidAndTheToggleStaysOff() async {
        let (model, driver) = running()
        await model.refreshUsbDebugging()
        driver.startError = AdbError.io(EADDRINUSE)
        model.setAdbOverNetwork(true)
        #expect(!model.session.adb && !driver.listening)
        #expect(model.session.adbProblem == "Port 5555 is already in use on this device.")
        driver.startError = POSIXError(.EPERM)
        model.setAdbOverNetwork(true)
        #expect(model.session.adbProblem == "ADB over network could not start.")
        driver.startError = nil
        model.setAdbOverNetwork(true)
        #expect(model.session.adb && model.session.adbProblem == nil)
        model.setAdbOverNetwork(false)
    }

    @Test func theEndOfARunClosesItAndForgetsTheSetting() async {
        let (model, driver) = running()
        await model.refreshUsbDebugging()
        model.setAdbOverNetwork(true)
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 10)
        #expect(!model.session.adb && !driver.listening)
        #expect(model.session.usbDebugging == nil)
    }

    @Test func aRestartClosesItToo() async {
        let (model, driver) = running()
        await model.refreshUsbDebugging()
        model.setAdbOverNetwork(true)
        model.restart()
        #expect(!model.session.adb && !driver.listening)
        #expect(model.session.usbDebugging == nil)
    }

    @Test func recoveryOffersNoAdb() async {
        let (model, driver) = running(.recovery)
        await model.refreshUsbDebugging()
        #expect(model.session.usbDebugging == nil)
        model.setAdbOverNetwork(true)
        #expect(!model.session.adb && driver.starts == 0)
    }

    @Test func thePreviewHasNoGuestToAskAndSaysYes() {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .preview,
            createRequirementBytes: 1
        )
        model.phase = .running(.normal)
        #expect(model.adbOverNetworkAvailable)
        model.setAdbOverNetwork(true)
        #expect(model.session.adb && model.session.adbAddress == SessionState.previewAddress)
        model.setAdbOverNetwork(false)
        model.session.usbDebugging = false
        #expect(!model.adbOverNetworkAvailable)
    }
}

struct PackageInstallMessagesTests {
    @Test func everyCodeInTheTableHasItsMessage() {
        let expected: [String: String] = [
            "PARSE_FAILED_NOT_APK": "The file isn\u{2019}t a valid app.",
            "FAILED_INVALID_APK": "The file isn\u{2019}t a valid app.",
            "PARSE_FAILED_BAD_MANIFEST": "The file isn\u{2019}t a valid app.",
            "PARSE_FAILED_MANIFEST_MALFORMED": "The file isn\u{2019}t a valid app.",
            "PARSE_FAILED_UNEXPECTED_EXCEPTION": "The file isn\u{2019}t a valid app.",
            "PARSE_FAILED_NO_CERTIFICATES": "The app isn\u{2019}t signed correctly.",
            "PARSE_FAILED_INCONSISTENT_CERTIFICATES": "The app isn\u{2019}t signed correctly.",
            "PARSE_FAILED_CERTIFICATE_ENCODING": "The app isn\u{2019}t signed correctly.",
            "FAILED_UPDATE_INCOMPATIBLE": "A version of this app signed by someone else is installed. Uninstall it in the guest first.",
            "FAILED_SHARED_USER_INCOMPATIBLE": "A version of this app signed by someone else is installed. Uninstall it in the guest first.",
            "FAILED_VERSION_DOWNGRADE": "A newer version of this app is already installed.",
            "FAILED_OLDER_SDK": "This app needs a newer system than this guest runs.",
            "FAILED_DEPRECATED_SDK_VERSION": "This app was made for a system version that this guest no longer installs.",
            "FAILED_NO_MATCHING_ABIS": "This app has no code for this device\u{2019}s processor.",
            "FAILED_INSUFFICIENT_STORAGE": "The guest doesn\u{2019}t have enough free space. Free some up in the guest, or give it more storage with Resize.",
            "FAILED_MISSING_SPLIT": "This app comes in several parts, which Andarium can\u{2019}t install yet.",
            "FAILED_MISSING_SHARED_LIBRARY": "The app needs a system library this guest doesn\u{2019}t have.",
            "FAILED_DUPLICATE_PERMISSION": "The app conflicts with an app that\u{2019}s already installed.",
            "FAILED_CONFLICTING_PROVIDER": "The app conflicts with an app that\u{2019}s already installed.",
            "FAILED_DUPLICATE_PACKAGE": "The app conflicts with an app that\u{2019}s already installed.",
            "FAILED_USER_RESTRICTED": "The guest refused to install the app.",
            "FAILED_VERIFICATION_FAILURE": "The guest refused to install the app.",
            "FAILED_ABORTED": "The guest refused to install the app.",
        ]
        #expect(Set(PackageInstallMessages.mappedCodes) == Set(expected.keys))
        for (code, message) in expected {
            #expect(PackageInstallMessages.message(forCode: code) == message, "\(code)")
            let output = "Failure [INSTALL_\(code): something detailed]\n"
            #expect(PackageInstallMessages.failure(inOutput: output) == message, "\(code)")
        }
    }

    @Test func anUnknownCodeIsNamed() {
        #expect(PackageInstallMessages.failure(inOutput: "Failure [INSTALL_FAILED_TEST_ONLY: x]\n")
            == "The guest couldn\u{2019}t install the app (FAILED_TEST_ONLY).")
    }

    @Test func successIsNil() {
        #expect(PackageInstallMessages.failure(inOutput: "Success\n") == nil)
        #expect(PackageInstallMessages.failure(inOutput: "Performing Streamed Install\nSuccess\n") == nil)
    }

    @Test func outputWithoutAVerdictIsAFailure() {
        #expect(PackageInstallMessages.failure(inOutput: "") == "The guest couldn\u{2019}t install the app.")
        #expect(PackageInstallMessages.failure(inOutput: "Failure\n") == "The guest couldn\u{2019}t install the app.")
    }

    @Test func theCodeIsReadWhateverFollowsIt() {
        #expect(PackageInstallMessages.code(inFailure: "Failure [INSTALL_FAILED_OLDER_SDK]") == "FAILED_OLDER_SDK")
        #expect(PackageInstallMessages.code(inFailure: "Failure [FAILED_ABORTED: Session was abandoned]") == "FAILED_ABORTED")
        #expect(PackageInstallMessages.code(inFailure: "Failure") == nil)
    }

    @Test func theInstallServiceHasItsArguments() {
        #expect(ApkInstallOperation.service(size: 1234) == "abb_exec:package\0install\0-r\0-t\0-S\0" + "1234")
    }
}
