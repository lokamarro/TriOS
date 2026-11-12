// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The settings of a launch, answered as the application's environment
/// reader would.
private func launch(
    _ settings: [LaunchOverrides.Setting: String], processorCount: Int = 4
) -> LaunchOverrides {
    LaunchOverrides(processorCount: processorCount) { settings[$0] }
}

struct LaunchOverridesTests {
    @Test func noSettingAsksNothing() {
        let overrides = launch([:], processorCount: 8)
        #expect(overrides.isEmpty)
        #expect(overrides.start == nil)
        #expect(!overrides.erase)
    }

    @Test func readsTheSharedSettings() {
        let overrides = launch(
            [
                .vCPUCount: "2",
                .memoryMiB: "1536",
                .diskGiB: "32",
                .retina: "1",
            ],
            processorCount: 6
        )
        #expect(overrides.vCPUCount == 2)
        #expect(overrides.memoryMiB == 1536)
        #expect(overrides.capacityGiB == 32)
        #expect(overrides.retina == true)
    }

    @Test func aCapacityTheLaunchStatesIsHeldToTheCarriedSystemsDisk() {
        var overrides = LaunchOverrides()
        overrides.capacityGiB = 100
        let host = HostDevice(
            name: "iPad", processorCount: 4,
            memory: MemoryPolicy(maximumsMiB: [4096, 4096, 3840, 3776]), freeStorageBytes: 200_000_000_000
        )
        #expect(overrides.applied(to: Guest.fresh(on: host), on: host).effectiveCapacityGiB == 100)
        let held = overrides.applied(to: Guest.fresh(on: host), on: host, capacityMaximumGiB: 64)
        #expect(held.effectiveCapacityGiB == 64)
    }

    @Test func aStoredFollowWindowValueIsIgnored() throws {
        // The guest's display always follows the window: an earlier version's
        // stored choice is read past, not migrated.
        let old = Data(#"{"retina":true,"followWindow":false}"#.utf8)
        #expect(try JSONDecoder().decode(DisplayOptions.self, from: old) == DisplayOptions(chosenRetina: true))
    }

    @Test func retinaFollowsTheMemoryTheSystemLetsTheAppUse() {
        let gib = UInt64(1) << 30
        // An iPad mini 6 holds the application to 2,867 MiB.
        #expect(!DisplayOptions.defaultRetina(memoryLimitBytes: 2867 << 20))
        #expect(!DisplayOptions.defaultRetina(memoryLimitBytes: 4 * gib))
        #expect(DisplayOptions.defaultRetina(memoryLimitBytes: 4 * gib + 1))
        #expect(DisplayOptions.defaultRetina(memoryLimitBytes: 6 * gib))
        // A Mac sets none.
        #expect(DisplayOptions.defaultRetina(memoryLimitBytes: 0))
    }

    @Test func aChoiceOfRetinaIsNeverOverriddenByTheDefault() {
        let gib = UInt64(1) << 30
        #expect(DisplayOptions().retina(memoryLimitBytes: 8 * gib))
        #expect(!DisplayOptions().retina(memoryLimitBytes: 2 * gib))
        #expect(!DisplayOptions(chosenRetina: false).retina(memoryLimitBytes: 8 * gib))
        #expect(DisplayOptions(chosenRetina: true).retina(memoryLimitBytes: 2 * gib))
    }

    @Test func aChosenValueSurvivesTheStoredForm() throws {
        for options in [DisplayOptions(), DisplayOptions(chosenRetina: true), DisplayOptions(chosenRetina: false)] {
            let data = try JSONEncoder().encode(options)
            #expect(try JSONDecoder().decode(DisplayOptions.self, from: data) == options)
        }
    }

    @Test func aStoredFalseFromBeforeTheDefaultIsNotAChoice() throws {
        let old = Data(#"{"retina":false}"#.utf8)
        #expect(try JSONDecoder().decode(DisplayOptions.self, from: old) == DisplayOptions())
        #expect(try JSONDecoder().decode(DisplayOptions.self, from: Data("{}".utf8)) == DisplayOptions())
    }

    @Test func ignoresValuesOutOfRangeOrOffStep() {
        let overrides = launch(
            [
                .vCPUCount: "9",
                .memoryMiB: "1000",
                .diskGiB: "x",
                .retina: "2",
            ],
            processorCount: 6
        )
        #expect(overrides.isEmpty)
    }

    /// The application passes the host's vCPU limit as `processorCount`, so
    /// the vCPU count states 1 through the limit and nothing above it.
    @Test func theVCPUCountIsBoundedByTheHostsLimit() {
        func stated(_ value: String, limit: Int) -> Int? {
            launch([.vCPUCount: value], processorCount: limit).vCPUCount
        }
        for limit in [6, 8] {
            #expect(stated("1", limit: limit) == 1)
            #expect(stated(String(limit), limit: limit) == limit)
            #expect(stated(String(limit + 1), limit: limit) == nil)
            #expect(stated("0", limit: limit) == nil)
            #expect(stated("many", limit: limit) == nil)
        }
        // Ten is no longer offered, whatever the device has.
        #expect(stated("10", limit: 8) == nil)
        #expect(stated("20", limit: 8) == nil)
    }

    @Test func startModes() {
        #expect(launch([.start: "normal"]).start == .normal)
        #expect(launch([.start: "recovery"]).start == .recovery)
        #expect(launch([.start: "other"]).start == nil)
        #expect(launch([.bootMode: "recovery"]).start == .recovery)
        #expect(launch([.bootMode: "normal"]).start == nil)
    }

    @Test func erase() {
        #expect(launch([.erase: "1"]).erase)
        #expect(!launch([.erase: "0"]).erase)
        #expect(!launch([:]).erase)
    }

    @Test func statistics() {
        #expect(launch([.stats: "1"]).statistics == true)
        #expect(launch([.stats: "0"]).statistics == false)
        // Neither stated: the person's own choice stands.
        #expect(launch([.stats: "yes"]).statistics == nil)
        #expect(launch([:]).statistics == nil)
        #expect(!launch([.stats: "1"]).isEmpty)
        #expect(!launch([.stats: "0"]).isEmpty)
    }

    @Test func isolatedInstanceNamesCannotEscapeTheirDirectory() {
        #expect(launch([.instance: "armeabi-legacy_1"]).instance == "armeabi-legacy_1")
        for name in ["", ".", "..", "../Guest", "/tmp/guest", "a/b", "a b", "游戏", String(repeating: "a", count: 65)] {
            let overrides = launch([.instance: name, .start: "normal", .erase: "1"])
            #expect(overrides.instance == nil)
            #expect(overrides.start == nil)
            #expect(!overrides.erase)
        }
    }

    @Test func appliesToAGuest() {
        var overrides = LaunchOverrides()
        overrides.vCPUCount = 8
        overrides.memoryMiB = 2048
        overrides.capacityGiB = 24
        let host = HostDevice(
            name: "iPad", processorCount: 4,
            memory: MemoryPolicy(maximumsMiB: [4096, 4096, 3840, 3776]), freeStorageBytes: 200_000_000_000
        )
        let guest = overrides.applied(to: Guest.fresh(on: host), on: host)
        #expect(guest.cores == 4)
        #expect(guest.memoryMiB == 2048)
        #expect(guest.effectiveCapacityGiB >= 24)
    }
}

struct ReportTests {
    @Test func guestKeysUseLinuxCodes() {
        #expect(GuestKey.back.rawValue == 158)
        #expect(GuestKey.home.rawValue == 172)
        #expect(GuestKey.power.rawValue == 116)
    }

    @Test func bootStepsFollowMilestones() {
        #expect(BootSteps.current(preparingDisk: true, milestone: .starting) == 0)
        #expect(BootSteps.current(preparingDisk: false, milestone: .starting) == 1)
        #expect(BootSteps.current(preparingDisk: false, milestone: .kernelRunning) == 2)
        #expect(BootSteps.current(preparingDisk: false, milestone: .androidStarting) == 3)
    }

    @Test func everyBootHasFourStepsAndStaysInTheLastUntilTheGuestIsReady() {
        // Startup (Recovery on a recovery boot) is the last step, and nothing
        // before sys.boot_completed moves past it, the graphics service
        // included. Recovery has no step of its own for its first frame.
        #expect(BootSteps.count == 4)
        #expect(BootSteps.current(preparingDisk: false, milestone: .androidStarting) == 3)
        #expect(BootSteps.current(preparingDisk: false, milestone: .screenStarting) == 3)
        #expect(BootSteps.current(preparingDisk: false, milestone: .ready) == 4)
    }

    @Test func aTrackerIsReadyOnlyAtTheEnd() {
        var tracker = BootTracker()
        #expect(!tracker.isReady)
        tracker.reach(.screenStarting, atSeconds: 8)
        #expect(!tracker.isReady)
        tracker.reach(.ready, atSeconds: 12)
        #expect(tracker.isReady)
        tracker.restart()
        #expect(!tracker.isReady)
        #expect(tracker.milestone == .starting)
    }

    @Test func aStartFailureEndsAsAFailure() {
        let end = RunEnd(result: -1, startFailure: "the disk could not be written")
        #expect(!end.poweredOff)
        #expect(end.phase == .failed)
    }
}

@MainActor
private final class FakeEffects: PauseEffects {
    var calls: [String] = []
    var pendingResume: (@MainActor () -> Void)?
    /// What the guest's processor time reads, in turn, the last repeating: a
    /// suspended guest's stands still, a running one's runs. Nothing reads
    /// nil.
    var readings: [UInt64?] = []
    private var reads = 0
    /// Runs when a suspension is asked, so a test can end the pause under it.
    var atSuspend: (@MainActor () -> Void)?
    /// Whether a resume finishes at once, as the runtime's queue does.
    var resumesAtOnce = false

    func holdGuestGPU() { calls.append("hold") }
    func releaseGuestGPU() { calls.append("release") }
    func flushGuest(until deadline: DispatchTime?) {
        calls.append(deadline == nil ? "flush" : "flush-wait")
    }
    func quiesceGuest(reason: String, until deadline: DispatchTime?) {
        calls.append("quiesce(\(reason),\(deadline == nil ? "nowait" : "wait"))")
    }
    func resumeGuest(finished: @escaping @MainActor () -> Void) {
        calls.append("resume")
        pendingResume = finished
        if resumesAtOnce { finished() }
    }
    func suspendGuestResident(reason: String, finished: @escaping @MainActor () -> Void) {
        calls.append("suspend(\(reason),resident)")
        atSuspend?()
        finished()
    }
    func guestProcessorTime() async -> UInt64? {
        calls.append("read")
        guard !readings.isEmpty else { return nil }
        defer { reads += 1 }
        return readings[min(reads, readings.count - 1)]
    }
}

@MainActor
private func makeController(runClock: GuestRunClock? = nil) throws -> (PauseController, FakeEffects, PauseMarker) {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("andarium-pause-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let marker = PauseMarker(url: directory.appendingPathComponent("pause.json"))
    let effects = FakeEffects()
    let controller = PauseController(
        effects: effects, marker: marker, runClock: runClock, spaceWait: { _ in })
    return (controller, effects, marker)
}

@MainActor
struct PauseControllerTests {
    @Test func backgroundHoldsTheGPUBeforePausing() throws {
        let (controller, effects, marker) = try makeController()
        controller.guestDidStart()
        controller.didEnterBackground()
        #expect(effects.calls == ["hold", "quiesce(background,wait)"])
        #expect(controller.phase == .paused)
        #expect(FileManager.default.fileExists(atPath: marker.url.path))
    }

    @Test func foregroundReleasesThenResumesAndKeepsTheMarkerUntilBack() throws {
        let (controller, effects, marker) = try makeController()
        controller.guestDidStart()
        controller.didEnterBackground()
        effects.calls.removeAll()
        controller.willEnterForeground()
        #expect(effects.calls == ["release", "resume"])
        #expect(FileManager.default.fileExists(atPath: marker.url.path))
        effects.pendingResume?()
        #expect(!FileManager.default.fileExists(atPath: marker.url.path))
        #expect(controller.phase == .foreground)
    }

    @Test func nothingHappensWithoutAGuest() throws {
        let (controller, effects, marker) = try makeController()
        controller.willResignActive()
        controller.didEnterBackground()
        controller.willEnterForeground()
        #expect(effects.calls.isEmpty)
        #expect(!FileManager.default.fileExists(atPath: marker.url.path))
    }

    @Test func losingFocusOnlyFlushes() throws {
        let (controller, effects, _) = try makeController()
        controller.guestDidStart()
        controller.willResignActive()
        #expect(effects.calls == ["flush"])
        #expect(controller.phase == .foreground)
    }

    @Test func aSecondBackgroundEdgeIsIgnored() throws {
        let (controller, effects, _) = try makeController()
        controller.guestDidStart()
        controller.didEnterBackground()
        controller.didEnterBackground()
        #expect(effects.calls == ["hold", "quiesce(background,wait)"])
    }

    @Test func terminationDisarmsAndWritesBack() throws {
        let (controller, effects, marker) = try makeController()
        controller.guestDidStart()
        controller.didEnterBackground()
        controller.willTerminate()
        #expect(!FileManager.default.fileExists(atPath: marker.url.path))
        #expect(effects.calls == ["hold", "quiesce(background,wait)"])

        let (second, secondEffects, _) = try makeController()
        second.guestDidStart()
        second.willTerminate()
        #expect(secondEffects.calls == ["quiesce(terminating,wait)"])
    }

    @Test func theRunClockStopsWhileTheGuestIsPausedAndRunsAgainWhenItIsBack() throws {
        let clock = GuestRunClock()
        let (controller, effects, _) = try makeController(runClock: clock)
        controller.guestDidStart()
        controller.willResignActive()
        #expect(!clock.isPaused)
        controller.didEnterBackground()
        #expect(clock.isPaused)
        controller.willEnterForeground()
        // The guest is not back until the resume has finished.
        #expect(clock.isPaused)
        effects.pendingResume?()
        #expect(!clock.isPaused)
    }

    @Test func aQuickReturnKeepsTheClockStoppedUntilTheLastResume() throws {
        let clock = GuestRunClock()
        let (controller, effects, _) = try makeController(runClock: clock)
        controller.guestDidStart()
        controller.didEnterBackground()
        controller.willEnterForeground()
        let first = effects.pendingResume
        controller.didEnterBackground()
        controller.willEnterForeground()
        let second = effects.pendingResume
        first?()
        #expect(clock.isPaused)
        second?()
        #expect(!clock.isPaused)
    }

    @Test func terminatingAlsoStopsTheClock() throws {
        let clock = GuestRunClock()
        let (controller, _, _) = try makeController(runClock: clock)
        controller.guestDidStart()
        controller.willTerminate()
        #expect(clock.isPaused)
    }

    /// A guest that holds still: its processor time never moves.
    private static let still: [UInt64?] = [5_000_000_000]

    @Test func beginningASpacePauseStopsEveryEdgeFromTouchingTheGuest() throws {
        let clock = GuestRunClock()
        let (controller, effects, marker) = try makeController(runClock: clock)
        controller.guestDidStart()
        #expect(controller.beginSpacePause())
        // Nothing reached the runtime yet; the pause is already the person's.
        #expect(effects.calls.isEmpty)
        #expect(controller.isPausedForSpace)
        #expect(clock.isPaused)
        // The application is in the foreground: nothing says it was left.
        #expect(controller.phase == .foreground)
        #expect(!FileManager.default.fileExists(atPath: marker.url.path))
    }

    @Test func aSpacePauseBeginsOnceAndOnlyForAGuestThatRunsInTheForeground() throws {
        let (idle, idleEffects, _) = try makeController()
        #expect(!idle.beginSpacePause())
        #expect(idleEffects.calls.isEmpty)

        let (controller, _, _) = try makeController()
        controller.guestDidStart()
        #expect(controller.beginSpacePause())
        #expect(!controller.beginSpacePause())

        let (background, backgroundEffects, _) = try makeController()
        background.guestDidStart()
        background.didEnterBackground()
        backgroundEffects.calls.removeAll()
        #expect(!background.beginSpacePause())
        #expect(backgroundEffects.calls.isEmpty)
    }

    @Test func aGuestThatHoldsStillIsSuspendedResidentWithoutTheGPUHeld() async throws {
        let (controller, effects, _) = try makeController()
        effects.readings = Self.still
        controller.guestDidStart()
        controller.beginSpacePause()
        let outcome = await controller.settleSpacePause()
        #expect(outcome == .held)
        // One suspension that wrote no RAM, and readings until it held still;
        // no GPU hold and no hibernating quiesce.
        #expect(effects.calls.first == "suspend(low space,resident)")
        #expect(effects.calls.filter { $0 != "read" } == ["suspend(low space,resident)"])
        #expect(effects.calls.filter { $0 == "read" }.count == SuspensionCheck.stillIntervals + 1)
        #expect(controller.isPausedForSpace)
    }

    @Test func aGuestThatKeepsRunningIsSuspendedAgainOnceAndThenLeftRunning() async throws {
        let clock = GuestRunClock()
        let (controller, effects, _) = try makeController(runClock: clock)
        effects.resumesAtOnce = true
        // Every reading is more than the last: the guest runs.
        effects.readings = (0..<64).map { UInt64($0) * 100_000_000 }
        controller.guestDidStart()
        controller.beginSpacePause()
        let outcome = await controller.settleSpacePause()
        #expect(outcome == .running)
        // Suspend, wake, suspend: the retry, and no more.
        #expect(effects.calls.filter { $0 != "read" } == [
            "suspend(low space,resident)", "resume", "suspend(low space,resident)",
        ])
        // The guest runs and no longer counts as paused: the edges treat it
        // as any running guest, and the run clock runs.
        #expect(!controller.isPausedForSpace)
        #expect(!clock.isPaused)
        effects.calls.removeAll()
        controller.willResignActive()
        #expect(effects.calls == ["flush"])
    }

    @Test func aGuestThatHoldsStillAtTheRetryIsHeld() async throws {
        let (controller, effects, _) = try makeController()
        effects.resumesAtOnce = true
        // It runs through the first attempt's readings, then stands still.
        let first = (0..<SuspensionCheck.maximumReadings).map { UInt64($0) * 100_000_000 }
        effects.readings = first + [UInt64(10_000_000_000)]
        controller.guestDidStart()
        controller.beginSpacePause()
        let outcome = await controller.settleSpacePause()
        #expect(outcome == .held)
        #expect(effects.calls.filter { $0 != "read" } == [
            "suspend(low space,resident)", "resume", "suspend(low space,resident)",
        ])
        #expect(controller.isPausedForSpace)
    }

    @Test func aReadingThatIsNotAvailableIsNoConfirmation() async throws {
        let (controller, effects, _) = try makeController()
        effects.resumesAtOnce = true
        effects.readings = []
        controller.guestDidStart()
        controller.beginSpacePause()
        let outcome = await controller.settleSpacePause()
        #expect(outcome == .running)
        #expect(effects.calls.filter { $0 == "suspend(low space,resident)" }.count == SpacePauser.suspendAttempts)
    }

    @Test func aPauseThatEndsWhileItSettlesReportsNothing() async throws {
        let (controller, effects, _) = try makeController()
        effects.readings = Self.still
        effects.atSuspend = { controller.guestDidEnd() }
        controller.guestDidStart()
        controller.beginSpacePause()
        let outcome = await controller.settleSpacePause()
        #expect(outcome == nil)
        #expect(!controller.isPausedForSpace)
    }

    @Test func leavingTheForegroundWhileASuspensionFailsSuspendsTheGuestForTheBackground() async throws {
        let (controller, effects, _) = try makeController()
        effects.resumesAtOnce = true
        effects.readings = (0..<64).map { UInt64($0) * 100_000_000 }
        controller.guestDidStart()
        controller.beginSpacePause()
        controller.didEnterBackground()
        // The guest counts as paused for space, so the edge left it alone.
        #expect(effects.calls == ["hold"])
        let outcome = await controller.settleSpacePause()
        #expect(outcome == .running)
        #expect(effects.calls.contains("quiesce(background,nowait)"))
    }

    @Test func noEdgeWakesAGuestPausedForSpace() async throws {
        let (controller, effects, _) = try makeController()
        effects.readings = Self.still
        controller.guestDidStart()
        controller.beginSpacePause()
        _ = await controller.settleSpacePause()
        effects.calls.removeAll()
        // A flush is a console request, which would end the suspension.
        controller.willResignActive()
        #expect(effects.calls.isEmpty)
        controller.didEnterBackground()
        // The guest is suspended already: the edge holds the GPU and says
        // the application left, and writes nothing.
        #expect(effects.calls == ["hold"])
        #expect(controller.phase == .paused)
        controller.willEnterForeground()
        #expect(effects.calls == ["hold", "release"])
        #expect(controller.phase == .foreground)
        #expect(controller.isPausedForSpace)
    }

    @Test func resumingFromTheSpacePauseWakesTheGuestAndRestartsTheClock() async throws {
        let clock = GuestRunClock()
        let (controller, effects, marker) = try makeController(runClock: clock)
        effects.readings = Self.still
        controller.guestDidStart()
        controller.beginSpacePause()
        _ = await controller.settleSpacePause()
        effects.calls.removeAll()
        var finished = false
        controller.resumeFromSpacePause { finished = true }
        #expect(effects.calls == ["resume"])
        // The guest is not back until the resume has finished.
        #expect(clock.isPaused)
        #expect(!finished)
        effects.pendingResume?()
        #expect(finished)
        #expect(!clock.isPaused)
        #expect(!controller.isPausedForSpace)
        #expect(!FileManager.default.fileExists(atPath: marker.url.path))
        // An edge treats the guest as any running one again.
        controller.willResignActive()
        #expect(effects.calls == ["resume", "flush"])
    }

    @Test func aSpacePauseIsResumedOnlyOnce() async throws {
        let (controller, effects, _) = try makeController()
        effects.readings = Self.still
        controller.guestDidStart()
        #expect(!controller.resumeFromSpacePause {})
        #expect(effects.calls.isEmpty)
        controller.beginSpacePause()
        _ = await controller.settleSpacePause()
        effects.calls.removeAll()
        #expect(controller.resumeFromSpacePause {})
        #expect(!controller.resumeFromSpacePause {})
        #expect(effects.calls == ["resume"])
    }

    @Test func aGuestPausedForSpaceIsNotResumedOutOfTheForeground() async throws {
        let (controller, effects, _) = try makeController()
        effects.readings = Self.still
        controller.guestDidStart()
        controller.beginSpacePause()
        _ = await controller.settleSpacePause()
        controller.didEnterBackground()
        effects.calls.removeAll()
        #expect(!controller.resumeFromSpacePause {})
        #expect(effects.calls.isEmpty)
        #expect(controller.isPausedForSpace)
    }

    @Test func theMarkerSaysTheApplicationLeftAGuestPausedForSpace() async throws {
        let (controller, effects, marker) = try makeController()
        effects.readings = Self.still
        controller.guestDidStart()
        controller.beginSpacePause()
        _ = await controller.settleSpacePause()
        controller.didEnterBackground()
        #expect(FileManager.default.fileExists(atPath: marker.url.path))
        controller.willEnterForeground()
        #expect(!FileManager.default.fileExists(atPath: marker.url.path))
    }

    @Test func aGuestThatEndsWhilePausedForSpaceLeavesNothingPaused() throws {
        let clock = GuestRunClock()
        let (controller, effects, _) = try makeController(runClock: clock)
        controller.guestDidStart()
        controller.beginSpacePause()
        controller.guestDidEnd()
        #expect(!controller.isPausedForSpace)
        #expect(!clock.isPaused)
        // The next run pauses and resumes as any.
        controller.guestDidStart()
        effects.calls.removeAll()
        #expect(controller.beginSpacePause())
    }

    @Test func terminatingWhilePausedForSpaceWritesNothingMore() async throws {
        let (controller, effects, marker) = try makeController()
        effects.readings = Self.still
        controller.guestDidStart()
        controller.beginSpacePause()
        _ = await controller.settleSpacePause()
        effects.calls.removeAll()
        controller.willTerminate()
        #expect(effects.calls.isEmpty)
        #expect(!FileManager.default.fileExists(atPath: marker.url.path))
    }

    @Test func endedGuestIsNotPaused() throws {
        let (controller, effects, _) = try makeController()
        controller.guestDidStart()
        controller.guestDidEnd()
        controller.didEnterBackground()
        #expect(effects.calls.isEmpty)
    }
}

struct PauseMarkerTests {
    @Test func consumeReportsOnce() throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-marker-\(UUID().uuidString)")
        let marker = PauseMarker(url: directory.appendingPathComponent("nested/pause.json"))
        #expect(!marker.consume())
        #expect(marker.arm())
        #expect(marker.consume())
        #expect(!marker.consume())
    }

    @Test func unreadableFileStillCounts() throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-marker-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let url = directory.appendingPathComponent("pause.json")
        try Data("not json".utf8).write(to: url)
        #expect(PauseMarker(url: url).consume())
    }
}
