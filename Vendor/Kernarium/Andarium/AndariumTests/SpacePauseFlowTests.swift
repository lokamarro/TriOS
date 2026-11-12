// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// A guest paused because the device is nearly out of space, as the model
/// plays it against a stub runtime: what the pause lets go, what Resume and
/// Shut down do, and what a run ending or restarting leaves. The runtime's
/// own pause is proved by `SpacePauseGuestTests` on a real guest.
@MainActor
struct SpacePauseFlowTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]),
        freeStorageBytes: 40_000_000_000
    )

    private func running() -> (AppModel, StubDriver) {
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
        model.phase = .running(.normal)
        return (model, driver)
    }

    /// The guest paused and holding still, as the runtime reports it.
    private func pause(_ model: AppModel, freeBytes: UInt64) {
        model.guestPausingForSpace(freeBytes: freeBytes)
        model.guestPausedForSpace()
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return condition()
    }

    @Test func pausingStatesTheSpaceAndLetsGoOfWhatNeedsTheGuest() {
        let (model, driver) = running()
        model.session.usbDebugging = true
        model.setAdbOverNetwork(true)
        #expect(model.session.adb && driver.listening)
        // A bug report is under way, which waits for the guest.
        driver.bugReport = { _ in
            try await Task.sleep(for: .seconds(60))
            throw CancellationError()
        }
        model.createBugReport()
        #expect(model.busy != nil)

        pause(model, freeBytes: 800 << 20)

        #expect(model.spacePause == SpacePause(freeBytes: 800 << 20))
        #expect(model.host.freeStorageBytes == 800 << 20)
        // ADB over network ends with the guest paused: its connections would
        // wait on a guest that does not run.
        #expect(!model.session.adb && !driver.listening)
        #expect(model.busy == nil)
    }

    @Test func aGuestBeingPausedOffersNothingYet() async {
        let (model, driver) = running()
        model.guestPausingForSpace(freeBytes: 800 << 20)
        #expect(model.spacePause?.stage == .pausing)
        model.resumeAfterSpacePause()
        model.shutDownAfterSpacePause()
        model.checkSpaceAfterPauseFailed()
        try? await Task.sleep(for: .milliseconds(50))
        #expect(model.spacePause?.stage == .pausing && model.spacePause?.working == nil)
        #expect(driver.spaceResumes == 0 && driver.spaceShutDowns == 0 && driver.spaceChecks == 0)
        model.guestPausedForSpace()
        #expect(model.spacePause?.stage == .paused)
    }

    @Test func aGuestThatDidNotPauseIsNeverResumedOnlyShutDownOrChecked() async {
        let (model, driver) = running()
        model.guestPausingForSpace(freeBytes: 800 << 20)
        model.guestNotPausedForSpace()
        #expect(model.spacePause?.stage == .notPaused)
        #expect(model.spacePause?.title == "Android is still running")
        model.resumeAfterSpacePause()
        try? await Task.sleep(for: .milliseconds(50))
        #expect(driver.spaceResumes == 0)
        #expect(model.spacePause?.working == nil)
        // Shut down is offered: the guest is running and can be asked.
        model.shutDownAfterSpacePause()
        #expect(model.spacePause?.working == .shuttingDown)
        #expect(await eventually { driver.spaceShutDowns == 1 })
    }

    @Test func checkingAgainWithRoomLetsTheScreenGo() async {
        let (model, driver) = running()
        model.guestPausingForSpace(freeBytes: 800 << 20)
        model.guestNotPausedForSpace()
        driver.checkAnswers = true
        model.checkSpaceAfterPauseFailed()
        #expect(model.spacePause?.stage == .pausing)
        #expect(await eventually { model.spacePause == nil })
        #expect(driver.spaceChecks == 1)
    }

    @Test func checkingAgainWithoutRoomPausesTheGuestAgainAndTheRuntimeSaysHow() async {
        let (model, driver) = running()
        model.guestPausingForSpace(freeBytes: 800 << 20)
        model.guestNotPausedForSpace()
        driver.checkAnswers = false
        model.checkSpaceAfterPauseFailed()
        #expect(await eventually { driver.spaceChecks == 1 })
        // The screen waits for what the runtime reports of the new attempt.
        #expect(model.spacePause?.stage == .pausing)
        model.guestPausedForSpace()
        #expect(model.spacePause?.stage == .paused)
        // Nothing to check over a guest that holds still.
        model.checkSpaceAfterPauseFailed()
        #expect(driver.spaceChecks == 1)
    }

    @Test func aPausedGuestIsNotAskedItsUsbDebugging() async {
        let (model, driver) = running()
        driver.debugging = false
        pause(model, freeBytes: 800 << 20)
        await model.refreshUsbDebugging()
        #expect(model.session.usbDebugging == nil)
    }

    @Test func resumeWithRoomClearsThePause() async {
        let (model, driver) = running()
        pause(model, freeBytes: 800 << 20)
        driver.resumeAnswers = true
        model.resumeAfterSpacePause()
        #expect(model.spacePause?.working == .resuming)
        #expect(await eventually { model.spacePause == nil })
        #expect(driver.spaceResumes == 1)
    }

    @Test func resumeWithoutRoomStaysPausedAndStatesTheSpaceNow() async {
        let (model, driver) = running()
        pause(model, freeBytes: 800 << 20)
        driver.resumeAnswers = false
        // The driver reads the space again and leaves it with the model.
        model.host.freeStorageBytes = 1_200 << 20
        model.resumeAfterSpacePause()
        #expect(await eventually { model.spacePause?.working == nil })
        #expect(model.spacePause == SpacePause(freeBytes: 1_200 << 20, stillShort: true))
        // The person may try again, and is not refused for having tried.
        driver.resumeAnswers = true
        model.resumeAfterSpacePause()
        #expect(await eventually { model.spacePause == nil })
        #expect(driver.spaceResumes == 2)
    }

    @Test func aSecondResumeWhileOneIsUnderWayIsIgnored() async {
        let (model, driver) = running()
        pause(model, freeBytes: 800 << 20)
        model.resumeAfterSpacePause()
        model.resumeAfterSpacePause()
        model.shutDownAfterSpacePause()
        #expect(await eventually { model.spacePause == nil })
        #expect(driver.spaceResumes == 1)
        #expect(driver.spaceShutDowns == 0)
    }

    @Test func shutDownAsksTheRuntimeAndWaitsForTheRunToEnd() async {
        let (model, driver) = running()
        pause(model, freeBytes: 800 << 20)
        model.shutDownAfterSpacePause()
        #expect(model.spacePause?.working == .shuttingDown)
        #expect(await eventually { driver.spaceShutDowns == 1 })
        // The guest has been asked; the screen stays until the run ends.
        #expect(model.spacePause?.working == .shuttingDown)
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 90)
        #expect(model.spacePause == nil)
        #expect(model.phase == .home)
    }

    @Test func aShutDownThatFailsLeavesAnAlertAndNoPause() async {
        let (model, driver) = running()
        driver.shutDownError = AdbFailure(message: "Nothing answered.")
        pause(model, freeBytes: 800 << 20)
        model.shutDownAfterSpacePause()
        #expect(await eventually { model.alert != nil })
        #expect(model.alert?.title == "Android could not be shut down")
        // The runtime watches the space again and pauses once more if it is
        // still short; the model does not keep a pause the guest left.
        #expect(model.spacePause == nil)
    }

    @Test func aGuestThatPausesAgainKeepsItsNewScreen() async {
        let (model, driver) = running()
        pause(model, freeBytes: 800 << 20)
        driver.resumeAnswers = true
        model.resumeAfterSpacePause()
        // The runtime resumed and paused again before the model heard back.
        model.spacePause = SpacePause(freeBytes: 700 << 20)
        try? await Task.sleep(for: .milliseconds(100))
        #expect(model.spacePause == SpacePause(freeBytes: 700 << 20))
    }

    @Test func theRunEndingOrRestartingTakesThePauseAway() {
        let (model, _) = running()
        pause(model, freeBytes: 800 << 20)
        model.restart()
        #expect(model.spacePause == nil)
        pause(model, freeBytes: 800 << 20)
        model.runEnded(RunEnd(result: -11), uptimeSeconds: 10)
        #expect(model.spacePause == nil)
    }

    @Test func thePreviewPlaysThePauseAway() {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(
                version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KB pages",
                appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
        model.phase = .running(.normal)
        pause(model, freeBytes: 800 << 20)
        model.resumeAfterSpacePause()
        #expect(model.spacePause == nil)
    }
}
