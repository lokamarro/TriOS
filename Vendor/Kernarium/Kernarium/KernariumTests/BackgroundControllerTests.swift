// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
@testable import Kernarium

/// What the background policy asks for at each lifecycle edge, in order. The
/// order is the point: the host GPU is held before the guest can reach it
/// while it is revoked, and released only once the guest is paused or kept
/// alive.
@MainActor
struct BackgroundControllerTests {
    /// Waits, for at most two seconds, for something that reaches the main
    /// actor after the test yields to it.
    private func settle(_ condition: () -> Bool) async {
        let deadline = DispatchTime.now() + .seconds(2)
        while !condition() && DispatchTime.now() < deadline {
            try? await Task.sleep(for: .milliseconds(5))
        }
    }

    private final class Deadline: BackgroundCancellable {
        let action: @MainActor () -> Void
        var cancelled = false

        init(_ action: @escaping @MainActor () -> Void) {
            self.action = action
        }

        func cancel() { cancelled = true }
    }

    private final class Grace: BackgroundCancellable {
        let name: String
        weak var effects: Effects?

        init(_ name: String, _ effects: Effects) {
            self.name = name
            self.effects = effects
        }

        func cancel() { effects?.log.add("end \(name)") }
    }

    /// Records every request and lets a test fire the policy's timer.
    private final class Effects: BackgroundEffects {
        let log = CallLog()
        var calls: [String] {
            get { log.entries }
            set { log.entries = newValue }
        }
        /// The bound of the GPU hold, which fires when a test says so.
        let timer = ManualHoldTimer()
        let gpuHold: GuestGPUHold
        var pictureInPictureCanStart = false
        var pictureInPictureActive = false
        var jobActive = false
        var guestIsAndroid = false
        var deadlines: [Deadline] = []
        /// What the guest's display is doing when the policy first asks, and
        /// whether it goes off within the wait it is given.
        var guestDisplayActive: Bool? = true
        var displayGoesOff = true
        var automationHoldDelay: TimeInterval = 0
        /// The deadline of each wait for the display, and the time it was asked.
        var displayWaits: [(asked: DispatchTime, until: DispatchTime)] = []
        /// What the main thread is doing while it waits: a test runs the
        /// bound's firing here, from another thread, as the real one would.
        var whileFlushing: (() -> Void)?
        var whileWaitingForDisplay: (() -> Void)?

        init() {
            gpuHold = GuestGPUHold(
                gate: RecordingGate(log),
                timer: timer.timer
            )
        }

        func flushGuest(until deadline: DispatchTime?) {
            log.add("flush")
            whileFlushing?()
        }
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
            log.add("wait display off")
            displayWaits.append((DispatchTime.now(), deadline))
            whileWaitingForDisplay?()
            return displayGoesOff
        }
        func stopPictureInPicture() { log.add("stop pip") }
        func keepScheduled(named name: String) -> any BackgroundCancellable {
            log.add("begin \(name)")
            return Grace(name, self)
        }
        func schedule(
            after seconds: TimeInterval,
            _ action: @escaping @MainActor () -> Void
        ) -> any BackgroundCancellable {
            let deadline = Deadline(action)
            deadlines.append(deadline)
            return deadline
        }

        func expire() {
            for deadline in deadlines where !deadline.cancelled {
                deadline.action()
            }
        }
    }

    @Test
    func aJobKeepsTheGuestRunningWithTheGPUHeld() {
        let effects = Effects()
        effects.jobActive = true
        let policy = controller(.keepRunning, effects)

        policy.didEnterBackground()
        // iOS gives a continued task no GPU, so it is never released.
        #expect(effects.calls == ["hold", "flush", "trim on"])
        #expect(policy.phase == .jobRunning)

        policy.willEnterForeground()
        #expect(effects.calls.suffix(2) == ["trim off", "release"])
        #expect(!effects.calls.contains("resume"))
        #expect(!effects.calls.contains { $0.hasPrefix("throttle") })
        #expect(policy.phase == .foreground)
    }

    @Test
    func anAndroidJobTurnsTheScreenOffAndOnAgain() {
        let effects = Effects()
        effects.jobActive = true
        effects.guestIsAndroid = true
        let policy = controller(.keepRunning, effects)

        policy.didEnterBackground()
        // The screen goes off while the GPU still answers, and the GPU is held
        // once it has: a screen that draws behind a held GPU leaves the apps it
        // shows waiting on it.
        #expect(effects.calls == [
            "power", "wait display off", "hold", "flush", "trim on",
        ])

        // The screen comes on after the GPU is back, so it can be drawn.
        policy.willEnterForeground()
        #expect(effects.calls.suffix(3) == ["trim off", "release", "power"])
    }

    @Test
    func aScreenThatNeverGoesOffIsHeldAtTheGraceAndNotLater() throws {
        let effects = Effects()
        effects.jobActive = true
        effects.guestIsAndroid = true
        effects.displayGoesOff = false
        let policy = controller(.keepRunning, effects)

        let before = DispatchTime.now()
        policy.didEnterBackground()
        let after = DispatchTime.now()

        // The hold follows the wait that gave up, and the flush follows the hold.
        #expect(effects.calls == [
            "power", "wait display off", "hold", "flush", "trim on",
        ])
        #expect(policy.phase == .jobRunning)

        // The wait ends at the grace, counted from the edge, and never past the
        // edge handler's budget.
        let wait = try #require(effects.displayWaits.first)
        let grace = UInt64(1_500_000_000)
        let budget = UInt64(4_000_000_000)
        #expect(wait.until.uptimeNanoseconds >= before.uptimeNanoseconds + grace)
        #expect(wait.until.uptimeNanoseconds <= after.uptimeNanoseconds + grace)
        #expect(wait.until.uptimeNanoseconds < before.uptimeNanoseconds + budget)
    }

    @Test
    func aScreenThatIsOffAlreadyIsNotWokenAndNotWaitedFor() {
        let effects = Effects()
        effects.jobActive = true
        effects.guestIsAndroid = true
        effects.guestDisplayActive = false
        let policy = controller(.keepRunning, effects)

        policy.didEnterBackground()
        #expect(effects.calls == ["hold", "flush", "trim on"])

        // The user left it off, and the return leaves it off.
        policy.willEnterForeground()
        #expect(!effects.calls.contains("power"))
    }

    @Test
    func aScreenWhoseStateIsUnknownIsTurnedOffAndWaitedFor() {
        let effects = Effects()
        effects.jobActive = true
        effects.guestIsAndroid = true
        effects.guestDisplayActive = nil
        let policy = controller(.keepRunning, effects)

        policy.didEnterBackground()
        #expect(effects.calls == [
            "power", "wait display off", "hold", "flush", "trim on",
        ])
    }

    @Test
    func theOtherModesHoldBeforeAnythingElse() {
        // Only a job on an Android guest turns the screen off first.
        let pausing = Effects()
        pausing.guestIsAndroid = true
        let pausePolicy = controller(.off, pausing)
        pausePolicy.didEnterBackground()
        #expect(pausing.calls.first == "hold")
        #expect(!pausing.calls.contains("wait display off"))

        let window = Effects()
        window.guestIsAndroid = true
        window.pictureInPictureCanStart = true
        let windowPolicy = controller(.pictureInPicture, window)
        windowPolicy.didEnterBackground()
        #expect(window.calls.first == "hold")
        #expect(!window.calls.contains("wait display off"))
        #expect(!window.calls.contains("power"))
    }

    /// A launch that asks for the hold after a delay gets it then, and the
    /// edge waits for no screen.
    @Test
    func anAutomationHoldDelayMovesTheHoldOffTheEdge() async {
        let effects = Effects()
        effects.jobActive = true
        effects.automationHoldDelay = 2.5
        let policy = controller(.keepRunning, effects)

        policy.didEnterBackground()
        #expect(effects.calls == [
            "begin gpu-hold-delay", "flush", "trim on",
        ])
        #expect(effects.timer.delays == [2.5])
        #expect(policy.phase == .jobRunning)

        effects.timer.fire()
        await settle { effects.calls.contains("end gpu-hold-delay") }
        #expect(Array(effects.calls.dropFirst(3)) == ["hold", "end gpu-hold-delay"])

        policy.willEnterForeground()
        #expect(effects.calls.suffix(2) == ["trim off", "release"])
    }

    /// The bound is a timer off the main thread. The guest's storage settles
    /// on a thread of its own while the main thread waits for it, and a hold
    /// queued behind that wait landed 2.5 s after the edge, 0.2 s before iOS
    /// denied the GPU.
    @Test
    func theHoldLandsWhileTheMainThreadWaitsForTheStorage() async {
        let effects = Effects()
        effects.jobActive = true
        effects.automationHoldDelay = 2.5
        effects.whileFlushing = {
            // The bound fires from another thread, as a dispatch timer does,
            // and the main thread stays in the flush until it has.
            DispatchQueue.global().sync { effects.timer.fire() }
            effects.log.add("flush ends")
        }
        let policy = controller(.keepRunning, effects)

        policy.didEnterBackground()
        #expect(effects.calls == [
            "begin gpu-hold-delay", "flush", "hold", "flush ends", "trim on",
        ])
        await settle { effects.calls.contains("end gpu-hold-delay") }
        #expect(effects.calls.contains("end gpu-hold-delay"))
    }

    /// Without a delay the bound is armed before the main thread can wait for
    /// anything, and the hold lands at it when the screen has not gone off.
    @Test
    func theHoldIsBoundedBeforeTheScreenIsWaitedFor() async {
        let effects = Effects()
        effects.jobActive = true
        effects.guestIsAndroid = true
        effects.displayGoesOff = false
        var armedWhenWaiting: [TimeInterval] = []
        effects.whileWaitingForDisplay = {
            armedWhenWaiting = effects.timer.delays
            // The main thread is stuck in the wait past the bound.
            DispatchQueue.global().sync { effects.timer.fire() }
        }
        let policy = controller(.keepRunning, effects)

        policy.didEnterBackground()
        #expect(armedWhenWaiting == [1.5])
        // The bound took the hold during the wait; the main thread finds it
        // taken and holds nothing a second time.
        #expect(effects.calls == [
            "power", "wait display off", "hold", "flush", "trim on",
        ])
        #expect(effects.timer.pending == 0)
    }

    /// An Android job that sees its screen go off holds then, and the bound
    /// has nothing left to do.
    @Test
    func aScreenThatGoesOffWithdrawsTheBound() {
        let effects = Effects()
        effects.jobActive = true
        effects.guestIsAndroid = true
        let policy = controller(.keepRunning, effects)

        policy.didEnterBackground()
        #expect(effects.timer.delays == [1.5])
        #expect(effects.timer.pending == 0)
        effects.timer.fire()
        #expect(effects.calls == [
            "power", "wait display off", "hold", "flush", "trim on",
        ])
    }

    @Test
    func aReturnBeforeTheDelayedHoldCancelsItAndReleasesNothing() {
        let effects = Effects()
        effects.jobActive = true
        effects.guestIsAndroid = true
        effects.automationHoldDelay = 4
        let policy = controller(.keepRunning, effects)

        policy.didEnterBackground()
        // The delay is armed first, and the screen is still turned off,
        // without a wait that would move the hold.
        #expect(effects.calls == [
            "begin gpu-hold-delay", "power", "flush", "trim on",
        ])

        policy.willEnterForeground()
        effects.timer.fire()
        #expect(!effects.calls.contains("hold"))
        #expect(!effects.calls.contains("release"))
        #expect(effects.calls.contains("end gpu-hold-delay"))
        #expect(effects.calls.suffix(2) == ["trim off", "power"])
    }

    /// The application's effects wait for the runtime's answer and stop at the
    /// deadline, and a runtime that cannot say is not taken for a screen that
    /// went off.
    @Test
    func theDisplayWaitEndsWhenTheRuntimeSaysOffOrAtTheDeadline() {
        let answers = DisplayAnswers([1, 1, 1, 0, 1])
        let effects = SystemBackgroundEffects(
            persistence: .shared,
            pictureInPicture: nil,
            displayActive: { answers.next() }
        )
        #expect(effects.waitForGuestDisplayOff(until: .now() + .seconds(5)))
        #expect(answers.asked == 4)

        let never = SystemBackgroundEffects(
            persistence: .shared,
            pictureInPicture: nil,
            displayActive: { -1 }
        )
        let started = DispatchTime.now()
        #expect(!never.waitForGuestDisplayOff(until: started + .milliseconds(100)))
        #expect(DispatchTime.now() >= started + .milliseconds(100))
        #expect(never.guestDisplayActive == nil)

        let on = SystemBackgroundEffects(
            persistence: .shared,
            pictureInPicture: nil,
            displayActive: { 1 }
        )
        #expect(on.guestDisplayActive == true)
        let off = SystemBackgroundEffects(
            persistence: .shared,
            pictureInPicture: nil,
            displayActive: { 0 }
        )
        #expect(off.guestDisplayActive == false)
        // Already off: the wait does not look twice.
        #expect(off.waitForGuestDisplayOff(until: .now()))
    }

    @Test
    func theHoldDelayIsTheRuntimesMillisecondsAsSeconds() {
        let delayed = SystemBackgroundEffects(
            persistence: .shared,
            pictureInPicture: nil,
            holdDelayMilliseconds: { 2500 }
        )
        #expect(delayed.automationHoldDelay == 2.5)
        let none = SystemBackgroundEffects(
            persistence: .shared,
            pictureInPicture: nil,
            holdDelayMilliseconds: { 0 }
        )
        #expect(none.automationHoldDelay == 0)
    }

    @Test
    func anAndroidJobThatEndedTurnsTheScreenOnAfterTheResume() {
        let effects = Effects()
        effects.jobActive = true
        effects.guestIsAndroid = true
        let policy = controller(.keepRunning, effects)
        policy.didEnterBackground()
        policy.jobDidEnd()
        effects.calls.removeAll()

        policy.willEnterForeground()
        #expect(effects.calls == ["release", "resume", "power"])
    }

    @Test
    func aGuestThatIsNotAndroidIsNeverSentThePowerKey() {
        let effects = Effects()
        effects.jobActive = true
        let policy = controller(.keepRunning, effects)

        policy.didEnterBackground()
        policy.willEnterForeground()
        #expect(!effects.calls.contains("power"))
    }

    @Test
    func anAndroidGuestThatPausesKeepsItsScreen() {
        let effects = Effects()
        effects.guestIsAndroid = true
        let policy = controller(.off, effects)

        policy.didEnterBackground()
        policy.willEnterForeground()
        #expect(!effects.calls.contains("power"))
    }

    @Test
    func aKeepRunningGuestWithNoJobPausesLikeAnyOther() {
        let effects = Effects()
        let policy = controller(.keepRunning, effects)

        policy.didEnterBackground()
        #expect(effects.calls == ["hold", "quiesce background"])
        #expect(policy.phase == .quiesced)
    }

    @Test
    func aJobThatEndsInTheBackgroundPausesTheGuestAndReturnResumesIt() {
        let effects = Effects()
        effects.jobActive = true
        let policy = controller(.keepRunning, effects)
        policy.didEnterBackground()
        effects.calls.removeAll()

        policy.jobDidEnd()
        #expect(effects.calls == ["trim off", "quiesce job ended"])
        #expect(policy.phase == .quiesced)

        policy.willEnterForeground()
        #expect(effects.calls.suffix(2) == ["release", "resume"])
        #expect(policy.phase == .foreground)
    }

    @Test
    func aJobThatEndsInTheForegroundChangesNothing() {
        let effects = Effects()
        effects.jobActive = true
        let policy = controller(.keepRunning, effects)

        policy.jobDidEnd()
        #expect(effects.calls.isEmpty)
        #expect(policy.phase == .foreground)
    }

    @Test
    func aJobDoesNotKeepAGuestThatChoseTheWindow() {
        let effects = Effects()
        effects.jobActive = true
        let policy = controller(.off, effects)

        policy.didEnterBackground()
        #expect(effects.calls == ["hold", "quiesce background"])
    }

    /// The runtime's answers for the guest's display, one per question, from
    /// any thread; the last one repeats.
    private nonisolated final class DisplayAnswers: @unchecked Sendable {
        private let lock = NSLock()
        private var answers: [Int32]
        private(set) var asked = 0

        init(_ answers: [Int32]) {
            self.answers = answers
        }

        func next() -> Int32 {
            lock.lock()
            defer { lock.unlock() }
            asked += 1
            return answers.count > 1 ? answers.removeFirst() : answers[0]
        }
    }

    /// What reached the runtime's page-cache trimming switch, from any thread.
    private nonisolated final class TrimmingSwitch: @unchecked Sendable {
        private let lock = NSLock()
        private var settings: [Int32] = []
        private var onMainThread = false

        func set(_ enabled: Int32) -> Int32 {
            lock.withLock {
                settings.append(enabled)
                onMainThread = onMainThread || Thread.isMainThread
            }
            return 0
        }

        var received: [Int32] { lock.withLock { settings } }
        var reachedOnMainThread: Bool { lock.withLock { onMainThread } }
    }

    private func controller(
        _ mode: BackgroundMode,
        _ effects: Effects
    ) -> BackgroundController {
        BackgroundController(effects: effects, mode: { mode })
    }

    @Test
    func aGuestThatChoseNothingIsHeldThenQuiescedAndResumedOnReturn() {
        let effects = Effects()
        let policy = controller(.off, effects)

        policy.didEnterBackground()
        #expect(effects.calls == ["hold", "quiesce background"])
        #expect(policy.phase == .quiesced)

        policy.willEnterForeground()
        #expect(effects.calls == [
            "hold", "quiesce background", "release", "resume",
        ])
        #expect(policy.phase == .foreground)
    }

    @Test
    func aWindowThatStartsKeepsTheGuestRunningAndReleasesTheGPU() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        let policy = controller(.pictureInPicture, effects)

        policy.didEnterBackground()
        #expect(effects.calls == [
            "hold", "flush", "begin picture-in-picture",
        ])
        #expect(policy.phase == .awaitingPictureInPicture)

        policy.pictureInPictureDidStart()
        #expect(Array(effects.calls.dropFirst(3)) == [
            "end picture-in-picture", "release", "trim on", "throttle on",
        ])
        #expect(policy.phase == .keptRunning)
        #expect(!effects.calls.contains { $0.hasPrefix("quiesce") })

        policy.willEnterForeground()
        #expect(effects.calls.suffix(2) == ["throttle off", "trim off"])
        #expect(!effects.calls.contains("resume"))
        #expect(policy.phase == .foreground)
    }

    /// A restarted guest boots a new kernel with neither the page-cache
    /// trimming nor the throttled vCPUs the policy gave the old one, so a
    /// guest kept running in a window gets both again, and nothing else.
    @Test
    func aGuestKeptRunningInAWindowThatRestartsGetsItsEdgesAgain() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        let policy = controller(.pictureInPicture, effects)
        policy.didEnterBackground()
        policy.pictureInPictureDidStart()
        effects.calls.removeAll()

        policy.guestDidRestart()
        #expect(effects.calls == ["trim on", "throttle on"])
        #expect(policy.phase == .keptRunning)

        // The guest still leaves the background the way it entered it.
        policy.willEnterForeground()
        #expect(effects.calls.suffix(2) == ["throttle off", "trim off"])
        #expect(!effects.calls.contains("resume"))
    }

    /// iOS already runs a continued task at background priority, so a job
    /// gets the trimming alone, as it did when the job began.
    @Test
    func aJobThatKeepsTheGuestRunningGetsOnlyTheTrimmingAgain() {
        let effects = Effects()
        effects.jobActive = true
        let policy = controller(.keepRunning, effects)
        policy.didEnterBackground()
        effects.calls.removeAll()

        policy.guestDidRestart()
        #expect(effects.calls == ["trim on"])
        #expect(policy.phase == .jobRunning)
    }

    /// A guest in the foreground, or one the policy paused, wants neither
    /// edge: asking would trim the cache of a guest the user is using, or
    /// throttle one that is paused.
    @Test
    func aRestartAsksNothingOfAGuestThatIsInTheForegroundOrPaused() {
        let foreground = Effects()
        let inForeground = controller(.keepRunning, foreground)
        inForeground.guestDidRestart()
        #expect(foreground.calls.isEmpty)

        let paused = Effects()
        let policy = controller(.off, paused)
        policy.didEnterBackground()
        paused.calls.removeAll()
        policy.guestDidRestart()
        #expect(paused.calls.isEmpty)
        #expect(policy.phase == .quiesced)

        let waiting = Effects()
        waiting.pictureInPictureCanStart = true
        let waitingPolicy = controller(.pictureInPicture, waiting)
        waitingPolicy.didEnterBackground()
        waiting.calls.removeAll()
        waitingPolicy.guestDidRestart()
        #expect(waiting.calls.isEmpty)
        #expect(waitingPolicy.phase == .awaitingPictureInPicture)
    }

    /// The application's effects pass the policy's trimming edges to the
    /// runtime, in order and away from the main thread, since the switch may
    /// wait for the guest's console.
    @Test
    func theTrimmingEdgesReachTheRuntimeOffTheMainThread() async throws {
        let runtime = TrimmingSwitch()
        let effects = SystemBackgroundEffects(
            persistence: .shared,
            pictureInPicture: nil,
            pageCacheTrimming: { runtime.set($0) }
        )

        effects.setPageCacheTrimming(true)
        effects.setPageCacheTrimming(false)
        #expect(try await eventually { runtime.received.count == 2 })
        #expect(runtime.received == [1, 0])
        #expect(!runtime.reachedOnMainThread)
    }

    @Test
    func aWindowThatDoesNotStartInTimePausesTheGuest() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        let policy = controller(.pictureInPicture, effects)
        policy.didEnterBackground()

        effects.expire()
        #expect(Array(effects.calls.dropFirst(3)) == [
            "end picture-in-picture",
            "quiesce picture in picture did not start",
        ])
        #expect(policy.phase == .quiesced)
        // The GPU stayed held from the edge: nothing released it.
        #expect(!effects.calls.contains("release"))

        policy.willEnterForeground()
        #expect(effects.calls.suffix(2) == ["release", "resume"])
    }

    @Test
    func aWindowThatFailsToStartPausesTheGuest() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        let policy = controller(.pictureInPicture, effects)
        policy.didEnterBackground()

        policy.pictureInPictureDidFailToStart()
        #expect(policy.phase == .quiesced)
        #expect(effects.calls.contains(
            "quiesce picture in picture did not start"
        ))
        // The start deadline is withdrawn: it cannot pause the guest twice.
        effects.expire()
        #expect(effects.calls.filter { $0.hasPrefix("quiesce") }.count == 1)
    }

    @Test
    func closingTheWindowInTheBackgroundHoldsTheGPUAndPausesTheGuest() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        let policy = controller(.pictureInPicture, effects)
        policy.didEnterBackground()
        policy.pictureInPictureDidStart()
        effects.calls.removeAll()

        policy.pictureInPictureDidStop()
        #expect(effects.calls == [
            "throttle off", "trim off", "hold",
            "quiesce picture in picture closed",
        ])
        #expect(policy.phase == .quiesced)

        policy.willEnterForeground()
        #expect(effects.calls.suffix(2) == ["release", "resume"])
    }

    @Test
    func closingTheWindowInTheForegroundChangesNothing() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        let policy = controller(.pictureInPicture, effects)
        policy.didEnterBackground()
        policy.pictureInPictureDidStart()
        policy.willEnterForeground()
        effects.calls.removeAll()

        policy.pictureInPictureDidStop()
        #expect(effects.calls.isEmpty)
        #expect(policy.phase == .foreground)
    }

    @Test
    func aWindowThatIsAlreadyUpAtTheEdgeNeedsNoWait() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        effects.pictureInPictureActive = true
        let policy = controller(.pictureInPicture, effects)

        policy.didEnterBackground()
        #expect(effects.calls == [
            "hold", "flush", "release", "trim on", "throttle on",
        ])
        #expect(policy.phase == .keptRunning)
    }

    @Test
    func aGuestWhoseWindowCannotStartPausesLikeAnyOther() {
        let effects = Effects()
        effects.pictureInPictureCanStart = false
        let policy = controller(.pictureInPicture, effects)

        policy.didEnterBackground()
        #expect(effects.calls == ["hold", "quiesce background"])
        #expect(policy.phase == .quiesced)
    }

    @Test
    func returningWhileTheWindowIsStillAwaitedCancelsTheWait() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        let policy = controller(.pictureInPicture, effects)
        policy.didEnterBackground()

        policy.willEnterForeground()
        #expect(effects.calls.suffix(2) == ["end picture-in-picture", "release"])
        #expect(!effects.calls.contains("resume"))
        effects.expire()
        #expect(!effects.calls.contains {
            $0.hasPrefix("quiesce")
        })
        #expect(policy.phase == .foreground)
    }

    @Test
    func becomingActiveClosesAWindowThatIsStillUp() {
        let effects = Effects()
        let policy = controller(.pictureInPicture, effects)
        policy.didBecomeActive()
        #expect(effects.calls.isEmpty)

        effects.pictureInPictureActive = true
        policy.didBecomeActive()
        #expect(effects.calls == ["stop pip"])
    }

    @Test
    func aSecondEdgeWhileInTheBackgroundIsIgnored() {
        let effects = Effects()
        let policy = controller(.off, effects)
        policy.didEnterBackground()
        effects.calls.removeAll()
        policy.didEnterBackground()
        #expect(effects.calls.isEmpty)
    }

    // MARK: Space pause

    /// A guest paused for space is resident and suspended: an edge that wrote
    /// its storage back, quiesced it or resumed it would end the suspension,
    /// and a quiesce would write the RAM image the device has no room for.
    @Test
    func anEdgeLeavesAGuestPausedForSpaceAloneAndHoldsTheGPU() {
        let effects = Effects()
        let policy = controller(.off, effects)
        policy.spacePauseBegan()
        #expect(policy.spaceHeld)
        #expect(!policy.guestRuns)

        policy.didEnterBackground()
        #expect(effects.calls == ["hold"])
        #expect(policy.phase == .quiesced)

        // The return releases the GPU and wakes nothing.
        policy.willEnterForeground()
        #expect(effects.calls == ["hold", "release"])
        #expect(policy.phase == .foreground)
        #expect(policy.spaceHeld)
    }

    @Test
    func everyModeLeavesAGuestPausedForSpaceAlone() {
        for mode in BackgroundMode.allCases {
            let effects = Effects()
            effects.pictureInPictureCanStart = true
            effects.jobActive = true
            let policy = controller(mode, effects)
            policy.spacePauseBegan()
            policy.didEnterBackground()
            #expect(effects.calls == ["hold"], "\(mode)")
            #expect(policy.phase == .quiesced, "\(mode)")
            #expect(effects.deadlines.isEmpty, "\(mode)")
        }
    }

    /// The pause begins while the policy keeps the guest running unseen: what
    /// ran it ends first, so the suspension is not throttled, and the GPU of a
    /// window that closes is held.
    @Test
    func aPauseForSpaceEndsWhatKeepsTheGuestRunningUnseen() {
        let window = Effects()
        window.pictureInPictureCanStart = true
        window.pictureInPictureActive = true
        let windowPolicy = controller(.pictureInPicture, window)
        windowPolicy.didEnterBackground()
        windowPolicy.pictureInPictureDidStart()
        #expect(windowPolicy.phase == .keptRunning)
        window.calls.removeAll()

        windowPolicy.spacePauseBegan()
        #expect(window.calls == [
            "throttle off", "trim off", "hold", "stop pip",
        ])
        #expect(windowPolicy.phase == .quiesced)
        // Closing the window afterwards asks nothing more of the guest.
        windowPolicy.pictureInPictureDidStop()
        #expect(!window.calls.contains { $0.hasPrefix("quiesce") })

        let job = Effects()
        job.jobActive = true
        let jobPolicy = controller(.keepRunning, job)
        jobPolicy.didEnterBackground()
        #expect(jobPolicy.phase == .jobRunning)
        job.calls.removeAll()

        jobPolicy.spacePauseBegan()
        // The job's hold stays, and the guest is no longer trimmed.
        #expect(job.calls == ["trim off"])
        #expect(jobPolicy.phase == .quiesced)
        // The job ending afterwards writes and quiesces nothing.
        jobPolicy.jobDidEnd()
        #expect(job.calls == ["trim off"])

        let waiting = Effects()
        waiting.pictureInPictureCanStart = true
        let waitingPolicy = controller(.pictureInPicture, waiting)
        waitingPolicy.didEnterBackground()
        #expect(waitingPolicy.phase == .awaitingPictureInPicture)
        waiting.calls.removeAll()

        waitingPolicy.spacePauseBegan()
        #expect(waiting.calls == ["end picture-in-picture"])
        #expect(waitingPolicy.phase == .quiesced)
        // The window the system starts later does not run the guest again.
        waitingPolicy.pictureInPictureDidStart()
        #expect(waitingPolicy.phase == .quiesced)
        #expect(!waiting.calls.contains("release"))
    }

    /// The window that the system starts as the app left is not wanted for a
    /// guest the pause ended before it opened, and no deadline wakes it.
    @Test
    func theWindowDeadlineOfAGuestPausedForSpaceDoesNothing() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        let policy = controller(.pictureInPicture, effects)
        policy.didEnterBackground()
        policy.spacePauseBegan()
        effects.calls.removeAll()

        effects.expire()
        #expect(effects.calls.isEmpty)
        #expect(policy.phase == .quiesced)
    }

    @Test
    func aPauseThatEndsInTheForegroundAsksNothingOfTheGuest() {
        let effects = Effects()
        let policy = controller(.off, effects)
        policy.spacePauseBegan()
        policy.spacePauseEnded(guestSuspended: true)
        #expect(!policy.spaceHeld)
        #expect(policy.guestRuns)
        #expect(effects.calls.isEmpty)

        // The edges are the policy's again.
        policy.didEnterBackground()
        #expect(effects.calls == ["hold", "quiesce background"])
        policy.willEnterForeground()
        #expect(effects.calls.suffix(2) == ["release", "resume"])
    }

    /// A guest that could not be suspended runs on. Behind an application that
    /// left the foreground it is paused as the background asks, which is what
    /// the edge would have done.
    @Test
    func aGuestThatCouldNotBePausedIsQuiescedByTheBackgroundAfterwards() {
        let effects = Effects()
        let policy = controller(.off, effects)
        policy.spacePauseBegan()
        policy.didEnterBackground()
        effects.calls.removeAll()

        policy.spacePauseEnded(guestSuspended: false)
        #expect(effects.calls == ["quiesce background"])
        #expect(policy.phase == .quiesced)
        #expect(!policy.spaceHeld)

        policy.willEnterForeground()
        #expect(effects.calls.suffix(2) == ["release", "resume"])
    }

    @Test
    func aGuestThatCouldNotBePausedInTheForegroundKeepsRunning() {
        let effects = Effects()
        let policy = controller(.off, effects)
        policy.spacePauseBegan()
        policy.spacePauseEnded(guestSuspended: false)
        #expect(effects.calls.isEmpty)
        #expect(policy.guestRuns)
        #expect(policy.isForeground)
    }

    @Test
    func theSpacePauseCallsAreIdempotent() {
        let effects = Effects()
        effects.jobActive = true
        let policy = controller(.keepRunning, effects)
        policy.spacePauseEnded(guestSuspended: true)
        #expect(effects.calls.isEmpty)

        policy.didEnterBackground()
        policy.spacePauseBegan()
        effects.calls.removeAll()
        policy.spacePauseBegan()
        #expect(effects.calls.isEmpty)
        #expect(policy.phase == .quiesced)
    }

    @Test
    func eachGuestKeepsItsOwnModeAndDefaultsToPausing() throws {
        let suite = "background-settings-\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let settings = BackgroundSettings(defaults: defaults)

        #expect(settings.mode(forGuest: "kde.erofs") == .off)
        settings.setMode(.pictureInPicture, forGuest: "kde.erofs")
        #expect(settings.mode(forGuest: "kde.erofs") == .pictureInPicture)
        #expect(settings.mode(forGuest: "android.kern") == .off)

        settings.setMode(.off, forGuest: "kde.erofs")
        #expect(settings.mode(forGuest: "kde.erofs") == .off)
        #expect(defaults.dictionaryRepresentation().keys.allSatisfy {
            !$0.hasPrefix(BackgroundSettings.keyPrefix)
        })

        // Automation can force a mode for one launch without touching what
        // the user chose; a mode this build does not know is ignored.
        let forced = BackgroundSettings(
            defaults: defaults,
            environment: [BackgroundSettings.environmentKey: "pictureInPicture"]
        )
        #expect(forced.mode(forGuest: "android.kern") == .pictureInPicture)
        let unknown = BackgroundSettings(
            defaults: defaults,
            environment: [BackgroundSettings.environmentKey: "someday"]
        )
        #expect(unknown.mode(forGuest: "android.kern") == .off)

        // A job's time is the guest's choice, a default otherwise, and
        // automation can force one for a launch.
        #expect(settings.limitMinutes(forGuest: "kde.erofs") == 30)
        settings.setLimitMinutes(120, forGuest: "kde.erofs")
        #expect(settings.limitMinutes(forGuest: "kde.erofs") == 120)
        #expect(settings.limitMinutes(forGuest: "android.kern") == 30)
        settings.setLimitMinutes(30, forGuest: "kde.erofs")
        #expect(settings.limitMinutes(forGuest: "kde.erofs") == 30)
        defaults.set(0, forKey: BackgroundSettings.limitKeyPrefix + "bad")
        #expect(settings.limitMinutes(forGuest: "bad") == 30)
        let short = BackgroundSettings(
            defaults: defaults,
            environment: [BackgroundSettings.limitEnvironmentKey: "2"]
        )
        #expect(short.limitMinutes(forGuest: "kde.erofs") == 2)
        let nonsense = BackgroundSettings(
            defaults: defaults,
            environment: [BackgroundSettings.limitEnvironmentKey: "-5"]
        )
        #expect(nonsense.limitMinutes(forGuest: "kde.erofs") == 30)

        // A kept guest is throttled unless automation turns that off.
        #expect(settings.throttlesKeptGuest)
        #expect(!BackgroundSettings(
            defaults: defaults,
            environment: [BackgroundSettings.throttleEnvironmentKey: "0"]
        ).throttlesKeptGuest)

        // A value from a newer build that this one does not know pauses.
        defaults.set("someday", forKey: BackgroundSettings.keyPrefix + "x")
        #expect(settings.mode(forGuest: "x") == .off)
    }

    @Test
    func aLimitIsTitledInWholeHoursWhereItIsOne() {
        for (minutes, title) in [
            (15, "15 minutes"), (30, "30 minutes"), (60, "1 hour"),
            (120, "2 hours"), (90, "90 minutes"), (1, "1 minute"),
            (24 * 60, "24 hours"),
        ] {
            #expect(BackgroundSettings.limitTitle(minutes: minutes) == title)
        }
    }
}
