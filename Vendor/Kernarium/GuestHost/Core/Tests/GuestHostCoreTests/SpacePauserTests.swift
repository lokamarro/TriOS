// SPDX-License-Identifier: GPL-2.0-or-later

import Testing

@testable import GuestHostCore

@MainActor
private final class FakeEffects: SpacePauseEffects {
    var calls: [String] = []
    /// What the guest's processor time reads, in turn, the last repeating: a
    /// suspended guest's stands still, a running one's runs. Nothing reads
    /// nil.
    var readings: [UInt64?] = []
    private var reads = 0
    /// Runs when a suspension is asked, so a test can end the pause under it.
    var atSuspend: (@MainActor () -> Void)?
    /// Whether a resume finishes at once, as the runtime's queue does.
    var resumesAtOnce = true
    var pendingResume: (@MainActor () -> Void)?

    func resumeGuest(finished: @escaping @MainActor () -> Void) {
        calls.append("resume")
        pendingResume = finished
        if resumesAtOnce { finished() }
    }

    func suspendGuestResident(reason: String, finished: @escaping @MainActor () -> Void) {
        calls.append("suspend(\(reason))")
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

/// A guest that runs: its processor time moves on at every reading.
private let runs: [UInt64?] = (0..<64).map { UInt64($0) * 50_000_000 }

@MainActor
struct SpacePauserTests {
    @Test func aPauseIsHeldFromBeginUntilItEnds() {
        let pauser = SpacePauser(effects: FakeEffects(), wait: { _ in })
        #expect(!pauser.isPaused)
        pauser.begin()
        #expect(pauser.isPaused)
        pauser.end()
        #expect(!pauser.isPaused)
    }

    @Test func aGuestThatHoldsStillIsHeldAfterOneSuspension() async {
        let effects = FakeEffects()
        effects.readings = [5_000_000_000]
        let pauser = SpacePauser(effects: effects, wait: { _ in })
        pauser.begin()
        #expect(await pauser.settle() == .held)
        #expect(pauser.isPaused)
        // One suspension, resident, and nothing else asked of the runtime.
        #expect(effects.calls.filter { $0 != "read" } == ["suspend(low space)"])
        #expect(effects.calls.filter { $0 == "read" }.count == SuspensionCheck.stillIntervals + 1)
    }

    @Test func aGuestThatKeepsRunningIsWokenAndSuspendedOnceMore() async {
        let effects = FakeEffects()
        effects.readings = runs
        let pauser = SpacePauser(effects: effects, wait: { _ in })
        pauser.begin()
        #expect(await pauser.settle() == .running)
        let asked = effects.calls.filter { $0 != "read" }
        #expect(asked == ["suspend(low space)", "resume", "suspend(low space)"])
        #expect(SpacePauser.suspendAttempts == 2)
        // A guest left running is no longer paused.
        #expect(!pauser.isPaused)
    }

    @Test func aGuestThatHoldsStillOnTheSecondAttemptIsHeld() async {
        let effects = FakeEffects()
        // Runs for the whole first check, then stands still.
        let first = Array(runs.prefix(SuspensionCheck.maximumReadings))
        effects.readings = first + [first.last!]
        let pauser = SpacePauser(effects: effects, wait: { _ in })
        pauser.begin()
        #expect(await pauser.settle() == .held)
        #expect(pauser.isPaused)
        #expect(effects.calls.filter { $0 != "read" } == ["suspend(low space)", "resume", "suspend(low space)"])
    }

    @Test func aGuestWhoseTimeCannotBeReadIsNotHeld() async {
        let effects = FakeEffects()
        let pauser = SpacePauser(effects: effects, wait: { _ in })
        pauser.begin()
        #expect(await pauser.settle() == .running)
        #expect(!pauser.isPaused)
    }

    @Test func aPauseEndedDuringTheSuspensionIsNotSettled() async {
        let effects = FakeEffects()
        effects.readings = [1]
        let pauser = SpacePauser(effects: effects, wait: { _ in })
        effects.atSuspend = { pauser.end() }
        pauser.begin()
        #expect(await pauser.settle() == nil)
        #expect(effects.calls == ["suspend(low space)"])
    }

    @Test func settlingWithoutAPauseDoesNothing() async {
        let effects = FakeEffects()
        let pauser = SpacePauser(effects: effects, wait: { _ in })
        #expect(await pauser.settle() == nil)
        #expect(effects.calls.isEmpty)
    }

    @Test func resumingWakesTheGuestOnceAndSaysWhenItIsBack() async {
        let effects = FakeEffects()
        effects.readings = [1]
        let pauser = SpacePauser(effects: effects, wait: { _ in })
        pauser.begin()
        _ = await pauser.settle()
        var back = false
        #expect(pauser.resume { back = true })
        #expect(back)
        #expect(!pauser.isPaused)
        #expect(effects.calls.filter { $0 == "resume" }.count == 1)
        // A second resume finds no pause and wakes nothing.
        var again = false
        #expect(!pauser.resume { again = true })
        #expect(!again)
        #expect(effects.calls.filter { $0 == "resume" }.count == 1)
    }

    @Test func resumingWithoutAPauseWakesNothing() {
        let effects = FakeEffects()
        let pauser = SpacePauser(effects: effects, wait: { _ in })
        #expect(!pauser.resume {})
        #expect(effects.calls.isEmpty)
    }
}
