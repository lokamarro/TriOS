// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import GuestHostCore
import Testing
@testable import Kernarium

/// The low-space pause of a running guest: when the watch pauses it, what the
/// background policy does meanwhile, and what the person's answers do. The
/// runtime, the free space and the clock are doubles; the intervals are
/// milliseconds.
@MainActor
struct GuestSpacePauseTests {
    // MARK: Doubles

    /// The device's free space, which the watch reads off the main thread.
    private final class FreeSpace: @unchecked Sendable {
        private let lock = NSLock()
        private var bytes: UInt64
        private var reads = 0

        init(mebibytes: UInt64) { bytes = mebibytes * Bytes.mib }

        func set(mebibytes: UInt64) {
            lock.withLock { bytes = mebibytes * Bytes.mib }
        }

        var readCount: Int { lock.withLock { reads } }

        func read() -> UInt64 {
            lock.withLock {
                reads += 1
                return bytes
            }
        }
    }

    /// The runtime's suspension, with the processor time a guest uses: it
    /// stands still while the guest is suspended, and keeps climbing when the
    /// guest ignores the suspension.
    private final class Suspension: SpacePauseEffects {
        var log: [String] = []
        /// Whether a suspension stops the guest's processor time.
        var holdsStill = true
        /// Whether the runtime has not returned yet: `finished` waits in
        /// `held` until a test releases it.
        var hangs = false
        var held: [@MainActor () -> Void] = []
        private var suspended = false
        private var time: UInt64 = 0

        func resumeGuest(finished: @escaping @MainActor () -> Void) {
            log.append("resume")
            suspended = false
            finished()
        }

        func suspendGuestResident(
            reason: String,
            finished: @escaping @MainActor () -> Void
        ) {
            log.append("suspend \(reason)")
            suspended = holdsStill
            if hangs {
                held.append(finished)
            } else {
                finished()
            }
        }

        func guestProcessorTime() async -> UInt64? {
            if !suspended { time += 500_000_000 }
            return time
        }

        func releaseHeld() {
            let waiting = held
            held = []
            waiting.forEach { $0() }
        }
    }

    private final class Run: SpaceRun {
        var isActive = true
        var guestName = "Android"
        var stops = 0

        func stop() { stops += 1 }
    }

    /// The policy's own effects, which a space-paused guest must not be
    /// asked for.
    private final class PolicyEffects: BackgroundEffects {
        let log = CallLog()
        let timer = ManualHoldTimer()
        let gpuHold: GuestGPUHold
        var jobActive = false
        var guestIsAndroid = false
        var guestDisplayActive: Bool? = true
        var automationHoldDelay: TimeInterval = 0
        var pictureInPictureCanStart = false
        var pictureInPictureActive = false

        init() {
            gpuHold = GuestGPUHold(
                gate: RecordingGate(log), timer: timer.timer
            )
        }

        func flushGuest(until deadline: DispatchTime?) { log.add("flush") }
        func quiesceGuest(reason: String, until deadline: DispatchTime?) {
            log.add("quiesce \(reason)")
        }
        func resumeGuest() { log.add("resume") }
        func setPageCacheTrimming(_ enabled: Bool) {
            log.add("trim \(enabled ? "on" : "off")")
        }
        func setBackgroundThrottle(_ enabled: Bool) {
            log.add("throttle \(enabled ? "on" : "off")")
        }
        func pressPowerKey() { log.add("power") }
        func waitForGuestDisplayOff(until deadline: DispatchTime) -> Bool {
            true
        }
        func stopPictureInPicture() { log.add("stop pip") }
        func keepScheduled(named name: String) -> any BackgroundCancellable {
            Noop()
        }
        func schedule(
            after seconds: TimeInterval,
            _ action: @escaping @MainActor () -> Void
        ) -> any BackgroundCancellable {
            Noop()
        }

        var calls: [String] { log.entries }
    }

    private final class Noop: BackgroundCancellable {
        func cancel() {}
    }

    private final class Count {
        var value = 0
    }

    @MainActor
    private struct Rig {
        let pause: GuestSpacePause
        let free: FreeSpace
        let suspension: Suspension
        let run: Run
        let effects: PolicyEffects
        let controller: BackgroundController
        let jobEnds: Count
        let windowRefreshes: Count

        var stage: SpacePause.Stage? { pause.pause?.stage }
    }

    private func makeRig(
        freeMebibytes: UInt64,
        mode: BackgroundMode = .off,
        holdsStill: Bool = true,
        harness: SpaceHarness? = nil
    ) -> Rig {
        let free = FreeSpace(mebibytes: freeMebibytes)
        let suspension = Suspension()
        suspension.holdsStill = holdsStill
        let run = Run()
        let effects = PolicyEffects()
        let controller = BackgroundController(
            effects: effects, mode: { mode }
        )
        let jobEnds = Count()
        let windowRefreshes = Count()
        let source: @Sendable () -> UInt64
        if let harness {
            source = { harness.freeSpace() }
        } else {
            source = { free.read() }
        }
        let pause = GuestSpacePause(
            effects: suspension,
            freeSpace: source,
            run: run,
            controller: { controller },
            endJob: { jobEnds.value += 1 },
            refreshWindow: { windowRefreshes.value += 1 },
            harness: harness,
            answerInterval: .milliseconds(10),
            idleInterval: .milliseconds(10),
            checkInterval: { _ in .milliseconds(10) },
            wait: { _ in }
        )
        return Rig(
            pause: pause, free: free, suspension: suspension, run: run,
            effects: effects, controller: controller, jobEnds: jobEnds,
            windowRefreshes: windowRefreshes
        )
    }

    /// Waits, for at most five seconds, for something that reaches the main
    /// actor after the test yields to it.
    private func until(_ condition: () -> Bool) async {
        let deadline = DispatchTime.now() + .seconds(5)
        while !condition() && DispatchTime.now() < deadline {
            try? await Task.sleep(for: .milliseconds(5))
        }
    }

    /// Lets the watch run for a while, to show that it does nothing.
    private func quiet() async {
        try? await Task.sleep(for: .milliseconds(150))
    }

    // MARK: Watch

    @Test
    func belowThePauseLineTheGuestIsSuspendedHeldAndTheWindowGoes() async {
        let rig = makeRig(freeMebibytes: 800)
        defer { rig.pause.runEnded() }
        rig.pause.runStarted()
        await until { rig.stage == .paused }

        #expect(rig.stage == .paused)
        #expect(rig.pause.pause?.ending == .stop)
        #expect(rig.pause.pause?.guest == "Android")
        #expect(rig.pause.pause?.freeBytes == 800 * Bytes.mib)
        #expect(rig.suspension.log == ["suspend low space"])
        #expect(rig.controller.spaceHeld)
        #expect(rig.pause.holdsGuest)
        // A job keeps a guest running, and a window shows one that runs.
        #expect(rig.jobEnds.value == 1)
        #expect(rig.windowRefreshes.value >= 1)
    }

    @Test
    func aRoomyDeviceIsReadAndNeverPaused() async {
        let rig = makeRig(freeMebibytes: 5_000)
        defer { rig.pause.runEnded() }
        rig.pause.runStarted()
        await until { rig.free.readCount >= 3 }

        #expect(rig.free.readCount >= 3)
        #expect(rig.pause.pause == nil)
        #expect(rig.suspension.log.isEmpty)
        #expect(!rig.controller.spaceHeld)
    }

    @Test
    func spaceThatFallsLaterPausesTheGuest() async {
        let rig = makeRig(freeMebibytes: 5_000)
        defer { rig.pause.runEnded() }
        rig.pause.runStarted()
        await until { rig.free.readCount >= 1 }
        #expect(rig.pause.pause == nil)

        rig.free.set(mebibytes: 700)
        await until { rig.stage == .paused }
        #expect(rig.stage == .paused)
        #expect(rig.pause.pause?.freeBytes == 700 * Bytes.mib)
    }

    @Test
    func theSpaceIsNotReadBetweenBoots() async {
        let rig = makeRig(freeMebibytes: 500)
        defer { rig.pause.runEnded() }
        rig.run.isActive = false
        rig.pause.runStarted()
        await quiet()
        #expect(rig.free.readCount == 0)
        #expect(rig.pause.pause == nil)

        rig.run.isActive = true
        await until { rig.stage == .paused }
        #expect(rig.stage == .paused)
    }

    /// No lifecycle edge may wake a space-paused guest, and the watch follows
    /// the same rules: a guest the policy keeps paused has nothing to read.
    @Test
    func theWatchLeavesAGuestThePolicyPausedAlone() async {
        let rig = makeRig(freeMebibytes: 500)
        defer { rig.pause.runEnded() }
        rig.controller.didEnterBackground()
        #expect(!rig.controller.guestRuns)
        rig.pause.runStarted()
        await quiet()
        #expect(rig.free.readCount == 0)
        #expect(rig.pause.pause == nil)
        #expect(rig.suspension.log.isEmpty)

        // The return makes the guest run, and the watch reads the space again.
        rig.controller.willEnterForeground()
        await until { rig.stage == .paused }
        #expect(rig.stage == .paused)
        #expect(rig.suspension.log == ["suspend low space"])
    }

    @Test
    func aRunStartedTwiceIsWatchedOnce() async {
        let rig = makeRig(freeMebibytes: 800)
        defer { rig.pause.runEnded() }
        rig.pause.runStarted()
        rig.pause.runStarted()
        await until { rig.stage == .paused }
        await quiet()
        #expect(rig.suspension.log == ["suspend low space"])
    }

    // MARK: Pausing

    @Test
    func aGuestThatDoesNotHoldStillIsCalledNotPausedAndRunsOn() async {
        let rig = makeRig(freeMebibytes: 800, holdsStill: false)
        defer { rig.pause.runEnded() }
        rig.pause.runStarted()
        await until { rig.stage == .notPaused }

        #expect(rig.stage == .notPaused)
        // One more attempt, behind a wake.
        #expect(rig.suspension.log == [
            "suspend low space", "resume", "suspend low space",
        ])
        // The policy treats the guest as running, and the watch leaves it be
        // while the screen is up.
        #expect(!rig.controller.spaceHeld)
        #expect(rig.controller.guestRuns)
        await quiet()
        #expect(rig.suspension.log.count == 3)
        #expect(rig.pause.holdsGuest)
    }

    @Test
    func theScreenOffersNothingWhileTheGuestIsBeingPaused() async {
        let rig = makeRig(freeMebibytes: 800)
        defer { rig.pause.runEnded() }
        rig.suspension.hangs = true
        rig.pause.runStarted()
        await until { !rig.suspension.held.isEmpty }

        #expect(rig.stage == .pausing)
        #expect(rig.controller.spaceHeld)
        rig.pause.resume()
        rig.pause.shutDown()
        rig.pause.checkAgain()
        #expect(rig.run.stops == 0)
        #expect(rig.pause.pause?.working == nil)
        #expect(rig.stage == .pausing)

        rig.suspension.releaseHeld()
        await until { rig.stage == .paused }
        #expect(rig.stage == .paused)
    }

    // MARK: Resume

    @Test
    func resumeReadsTheSpaceAgainAndWakesTheGuestOnlyWithRoom() async {
        let rig = makeRig(freeMebibytes: 800)
        defer { rig.pause.runEnded() }
        rig.pause.runStarted()
        await until { rig.stage == .paused }

        // Short still: it says what is free now and stays paused.
        rig.free.set(mebibytes: 1_200)
        rig.pause.resume()
        await until { rig.pause.pause?.stillShort == true }
        #expect(rig.pause.pause?.stillShort == true)
        #expect(rig.pause.pause?.freeBytes == 1_200 * Bytes.mib)
        #expect(rig.pause.pause?.working == nil)
        #expect(rig.stage == .paused)
        #expect(rig.suspension.log == ["suspend low space"])
        #expect(rig.controller.spaceHeld)

        // With room, the guest wakes, the screen goes and the policy is the
        // guest's again.
        rig.free.set(mebibytes: 2_000)
        rig.pause.resume()
        await until { rig.pause.pause == nil }
        #expect(rig.pause.pause == nil)
        #expect(rig.suspension.log == ["suspend low space", "resume"])
        #expect(!rig.controller.spaceHeld)
        #expect(!rig.pause.holdsGuest)
        #expect(rig.windowRefreshes.value >= 2)

        // The watch goes on over the guest that runs.
        await quiet()
        #expect(rig.pause.pause == nil)
    }

    /// An edge that comes while the screen reads the space leaves the guest
    /// paused: only the foreground wakes it.
    @Test
    func aGuestIsWokenInTheForegroundOnly() async {
        let rig = makeRig(freeMebibytes: 800)
        defer { rig.pause.runEnded() }
        rig.pause.runStarted()
        await until { rig.stage == .paused }

        rig.free.set(mebibytes: 3_000)
        rig.controller.didEnterBackground()
        rig.pause.resume()
        await until { rig.pause.pause?.working == nil }
        await quiet()
        #expect(rig.stage == .paused)
        #expect(rig.pause.pause?.working == nil)
        #expect(rig.suspension.log == ["suspend low space"])
        // The edge held the GPU and asked nothing else of the guest.
        #expect(rig.effects.calls == ["hold"])

        // The return asks nothing of it either, and Resume then wakes it.
        rig.controller.willEnterForeground()
        #expect(rig.effects.calls == ["hold", "release"])
        #expect(rig.suspension.log == ["suspend low space"])
        rig.pause.resume()
        await until { rig.pause.pause == nil }
        #expect(rig.suspension.log == ["suspend low space", "resume"])
        #expect(!rig.controller.spaceHeld)
    }

    @Test
    func everyBackgroundModeLeavesAPausedGuestSuspended() async {
        for mode in BackgroundMode.allCases {
            let rig = makeRig(freeMebibytes: 800, mode: mode)
            rig.effects.jobActive = true
            rig.effects.pictureInPictureCanStart = true
            rig.pause.runStarted()
            await until { rig.stage == .paused }

            rig.controller.didEnterBackground()
            rig.controller.willEnterForeground()
            rig.controller.didEnterBackground()
            await quiet()
            #expect(rig.suspension.log == ["suspend low space"], "\(mode)")
            #expect(
                !rig.effects.calls.contains {
                    $0 == "flush" || $0 == "resume" || $0.hasPrefix("quiesce")
                },
                "\(mode)"
            )
            rig.pause.runEnded()
        }
    }

    // MARK: Shut down

    @Test
    func shutDownEndsTheRunTheWayLeavingTheGuestDoes() async {
        let rig = makeRig(freeMebibytes: 800)
        defer { rig.pause.runEnded() }
        rig.pause.runStarted()
        await until { rig.stage == .paused }

        rig.pause.shutDown()
        // The suspension wrote the guest's storage back already, so nothing is
        // asked of the guest: the run's own stop is the whole of it.
        #expect(rig.run.stops == 1)
        #expect(rig.pause.pause?.working == .shuttingDown)
        #expect(rig.suspension.log == ["suspend low space"])
        #expect(rig.pause.pause?.detail.contains("What it had stored is kept") == true)

        rig.pause.shutDown()
        rig.pause.resume()
        #expect(rig.run.stops == 1)
        #expect(rig.pause.pause?.working == .shuttingDown)

        // The run returning takes the screen away and ends the hold.
        rig.run.isActive = false
        rig.pause.runEnded()
        #expect(rig.pause.pause == nil)
        #expect(!rig.controller.spaceHeld)
    }

    @Test
    func aGuestThatRunsOnCanBeShutDownToo() async {
        let rig = makeRig(freeMebibytes: 800, holdsStill: false)
        defer { rig.pause.runEnded() }
        rig.pause.runStarted()
        await until { rig.stage == .notPaused }

        rig.pause.shutDown()
        #expect(rig.run.stops == 1)
        #expect(rig.pause.pause?.working == .shuttingDown)
    }

    // MARK: Check again

    @Test
    func checkAgainClearsTheScreenWhenThereIsRoomNow() async {
        let rig = makeRig(freeMebibytes: 800, holdsStill: false)
        defer { rig.pause.runEnded() }
        rig.pause.runStarted()
        await until { rig.stage == .notPaused }

        rig.free.set(mebibytes: 3_000)
        rig.pause.checkAgain()
        await until { rig.pause.pause == nil }
        #expect(rig.pause.pause == nil)
        #expect(!rig.controller.spaceHeld)
        #expect(rig.controller.guestRuns)
    }

    @Test
    func checkAgainPausesTheGuestOnceMoreWhileItIsShort() async {
        let rig = makeRig(freeMebibytes: 800, holdsStill: false)
        defer { rig.pause.runEnded() }
        rig.pause.runStarted()
        await until { rig.stage == .notPaused }

        // This time the guest holds still.
        rig.suspension.holdsStill = true
        rig.free.set(mebibytes: 900)
        rig.pause.checkAgain()
        await until { rig.stage == .paused }
        #expect(rig.stage == .paused)
        #expect(rig.pause.pause?.freeBytes == 900 * Bytes.mib)
        #expect(rig.controller.spaceHeld)
    }

    // MARK: End of the run

    @Test
    func aRunThatEndsClearsTheScreenAndTheHold() async {
        let rig = makeRig(freeMebibytes: 800)
        rig.pause.runStarted()
        await until { rig.stage == .paused }
        #expect(rig.controller.spaceHeld)

        rig.run.isActive = false
        rig.pause.runEnded()
        #expect(rig.pause.pause == nil)
        #expect(!rig.controller.spaceHeld)
        #expect(!rig.pause.holdsGuest)

        // Another run is watched afresh.
        rig.free.set(mebibytes: 5_000)
        rig.run.isActive = true
        rig.pause.runStarted()
        await until { rig.free.readCount >= 2 }
        #expect(rig.pause.pause == nil)
        rig.pause.runEnded()
    }

    // MARK: Harness

    private func harnessDirectory() throws -> URL {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("space-harness-\(UUID().uuidString)")
        try FileManager.default.createDirectory(
            at: directory, withIntermediateDirectories: true
        )
        return directory
    }

    @Test
    func theHarnessStandsForTheDevicesFreeSpaceAndAnswersTheScreen() async throws {
        let directory = try harnessDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let harness = SpaceHarness(enabled: true, directory: directory)
        try "800".write(
            to: directory.appendingPathComponent(SpaceHarness.freeSpaceFile),
            atomically: true, encoding: .utf8
        )
        let rig = makeRig(freeMebibytes: 0, harness: harness)
        defer { rig.pause.runEnded() }
        rig.pause.runStarted()
        await until { rig.stage == .paused }
        #expect(rig.stage == .paused)

        // An answer the screen does not offer waits; Resume, once the space is
        // there, wakes the guest and the file goes.
        let answer = directory.appendingPathComponent(SpaceHarness.answerFile)
        try "check".write(to: answer, atomically: true, encoding: .utf8)
        await quiet()
        #expect(rig.stage == .paused)
        #expect(FileManager.default.fileExists(atPath: answer.path))

        try "3000".write(
            to: directory.appendingPathComponent(SpaceHarness.freeSpaceFile),
            atomically: true, encoding: .utf8
        )
        try "resume".write(to: answer, atomically: true, encoding: .utf8)
        await until { rig.pause.pause == nil }
        #expect(rig.pause.pause == nil)
        #expect(rig.suspension.log == ["suspend low space", "resume"])
        #expect(!FileManager.default.fileExists(atPath: answer.path))
    }

    @Test
    func theHarnessCanShutTheGuestDown() async throws {
        let directory = try harnessDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let harness = SpaceHarness(enabled: true, directory: directory)
        try "800".write(
            to: directory.appendingPathComponent(SpaceHarness.freeSpaceFile),
            atomically: true, encoding: .utf8
        )
        let rig = makeRig(freeMebibytes: 0, harness: harness)
        defer { rig.pause.runEnded() }
        rig.pause.runStarted()
        await until { rig.stage == .paused }

        try "shutdown".write(
            to: directory.appendingPathComponent(SpaceHarness.answerFile),
            atomically: true, encoding: .utf8
        )
        await until { rig.run.stops == 1 }
        #expect(rig.run.stops == 1)
    }

    @Test
    func aDisabledHarnessReadsAndAnswersNothing() throws {
        let directory = try harnessDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        try "1".write(
            to: directory.appendingPathComponent(SpaceHarness.freeSpaceFile),
            atomically: true, encoding: .utf8
        )
        try "resume".write(
            to: directory.appendingPathComponent(SpaceHarness.answerFile),
            atomically: true, encoding: .utf8
        )
        let harness = SpaceHarness(enabled: false, directory: directory)
        #expect(harness.freeSpace() != Bytes.mib)
        #expect(harness.answer() == nil)
    }

    @Test
    func theHarnessFilesAreParsedStrictly() throws {
        let directory = try harnessDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let harness = SpaceHarness(enabled: true, directory: directory)
        let space = directory.appendingPathComponent(SpaceHarness.freeSpaceFile)
        let answer = directory.appendingPathComponent(SpaceHarness.answerFile)

        try " 900\n".write(to: space, atomically: true, encoding: .utf8)
        #expect(harness.freeSpace() == 900 * Bytes.mib)
        // A number the harness cannot read leaves the device's own space.
        try "lots".write(to: space, atomically: true, encoding: .utf8)
        #expect(harness.freeSpace() != 900 * Bytes.mib)

        #expect(harness.answer() == nil)
        for (text, expected) in [
            ("resume\n", SpaceAnswer.resume), ("shutdown", .shutdown),
            ("check", .check),
        ] {
            try text.write(to: answer, atomically: true, encoding: .utf8)
            #expect(harness.answer() == expected)
        }
        try "maybe".write(to: answer, atomically: true, encoding: .utf8)
        #expect(harness.answer() == nil)

        harness.consumeAnswer()
        #expect(!FileManager.default.fileExists(atPath: answer.path))
    }

    // MARK: Start check

    @Test
    func aStartNeedsRoomForTheGuestsFirstWrites() {
        let image = GuestSelection.image(
            URL(fileURLWithPath: "/nowhere/kde.erofs"), flavor: .linux
        )
        let short = EmulatorLauncher.startShortage(
            for: image, freeBytes: Bytes.gib
        )
        #expect(short?.neededBytes == RunSpace.startReserveBytes)
        #expect(short?.freeBytes == Bytes.gib)
        #expect(
            EmulatorLauncher.startShortage(
                for: image, freeBytes: RunSpace.startReserveBytes
            ) == nil
        )
    }

    @Test
    func aBundleWithoutADataDiskNeedsTheFirstStartsRoom() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("start-space-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        let url = root.appendingPathComponent("guest.kern")
        try FileManager.default.createDirectory(
            at: url.appendingPathComponent("guest"),
            withIntermediateDirectories: true
        )
        let bundle = GuestBundle(url: url, flavor: .android)
        let selection = GuestSelection.bundle(bundle)

        // No data disk yet: the first start writes its first setup.
        let first = EmulatorLauncher.startShortage(
            for: selection, freeBytes: 2 * Bytes.gib
        )
        #expect(first?.neededBytes == RunSpace.firstStartReserveBytes)
        #expect(
            EmulatorLauncher.startShortage(
                for: selection, freeBytes: RunSpace.firstStartReserveBytes
            ) == nil
        )

        // With one, the ordinary start's room is enough.
        FileManager.default.createFile(
            atPath: bundle.dataDisk.path, contents: Data()
        )
        #expect(
            EmulatorLauncher.startShortage(
                for: selection, freeBytes: 2 * Bytes.gib
            ) == nil
        )
        #expect(
            EmulatorLauncher.startShortage(
                for: selection, freeBytes: Bytes.gib
            )?.neededBytes == RunSpace.startReserveBytes
        )
    }
}
