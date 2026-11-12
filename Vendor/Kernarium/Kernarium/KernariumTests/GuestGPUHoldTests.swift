// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
@testable import Kernarium

/// The hold on the guest's GPU requests has a bound that is a timer of its own,
/// so the hold lands whatever the main thread is doing. These tests drive the
/// hold directly; the policy's use of it is in `BackgroundControllerTests`.
struct GuestGPUHoldTests {
    private struct Rig {
        let log = CallLog()
        let timer = ManualHoldTimer()
        let hold: GuestGPUHold

        init() {
            hold = GuestGPUHold(gate: RecordingGate(log), timer: timer.timer)
        }
    }

    @Test
    func aHoldTakenIsTakenOnceAndReleasedOnce() {
        let rig = Rig()

        #expect(rig.hold.hold())
        #expect(!rig.hold.hold())
        #expect(rig.hold.isHeld)
        #expect(rig.log.entries == ["hold"])

        #expect(rig.hold.release())
        #expect(!rig.hold.release())
        #expect(!rig.hold.isHeld)
        #expect(rig.log.entries == ["hold", "release"])

        // A released hold can be taken again.
        #expect(rig.hold.hold())
        #expect(rig.log.entries == ["hold", "release", "hold"])
    }

    @Test
    func theBoundTakesTheHoldWhenItFires() {
        let rig = Rig()
        let landed = CallLog()

        rig.hold.arm(after: 1.5) { landed.add("landed") }
        #expect(rig.timer.delays == [1.5])
        #expect(rig.log.entries.isEmpty)

        rig.timer.fire()
        #expect(rig.log.entries == ["hold"])
        #expect(landed.entries == ["landed"])
        #expect(rig.hold.isHeld)
    }

    @Test
    func aHoldTakenFirstLeavesTheBoundNothingToDo() {
        let rig = Rig()
        let landed = CallLog()
        rig.hold.arm(after: 1.5) { landed.add("landed") }

        #expect(rig.hold.hold())
        #expect(rig.timer.pending == 0)
        rig.timer.fire()

        #expect(rig.log.entries == ["hold"])
        #expect(landed.entries.isEmpty)
    }

    /// A real timer can fire as it is withdrawn: its handler is already on its
    /// way when the withdrawal reaches it. The handler must not hold what the
    /// return has just released.
    @Test
    func aBoundThatFiresAsItIsWithdrawnHoldsNothing() {
        let rig = Rig()
        rig.timer.honorsWithdrawal = false
        let landed = CallLog()
        rig.hold.arm(after: 1.5) { landed.add("landed") }

        #expect(!rig.hold.release())
        rig.timer.fire()

        #expect(rig.log.entries.isEmpty)
        #expect(landed.entries.isEmpty)
        #expect(!rig.hold.isHeld)
    }

    @Test
    func aStaleBoundDoesNotHoldAfterAHoldAndARelease() {
        let rig = Rig()
        rig.timer.honorsWithdrawal = false
        rig.hold.arm(after: 1.5)

        #expect(rig.hold.hold())
        #expect(rig.hold.release())
        rig.timer.fire()

        #expect(rig.log.entries == ["hold", "release"])
        #expect(!rig.hold.isHeld)
    }

    @Test
    func armingAgainReplacesTheBound() {
        let rig = Rig()
        let landed = CallLog()
        rig.hold.arm(after: 1.5) { landed.add("first") }
        rig.hold.arm(after: 3) { landed.add("second") }
        #expect(rig.timer.delays == [1.5, 3])
        #expect(rig.timer.pending == 1)

        rig.timer.fire()
        #expect(rig.log.entries == ["hold"])
        #expect(landed.entries == ["second"])
    }

    /// Both paths racing for the hold take it once between them.
    @Test
    func theBoundAndTheHoldRacingTakeTheHoldOnce() async {
        for _ in 0..<200 {
            let rig = Rig()
            rig.hold.arm(after: 1.5)
            await withTaskGroup(of: Void.self) { group in
                group.addTask { rig.timer.fire() }
                group.addTask { rig.hold.hold() }
            }
            #expect(rig.log.entries == ["hold"])
            #expect(rig.hold.release())
        }
    }

    /// The point of the bound: the main thread is blocked, as it is while it
    /// waits for the guest's storage to settle, and the hold lands anyway. The
    /// production timer is the one under test here, so nothing is simulated.
    @MainActor
    @Test
    func theBoundHoldsWhileTheMainThreadIsBlocked() {
        final class Gate: GuestGPUGate, @unchecked Sendable {
            let held = DispatchSemaphore(value: 0)
            private let lock = NSLock()
            private var onMain: Bool?

            var heldOnMainThread: Bool? { lock.withLock { onMain } }

            func hold() {
                lock.withLock { onMain = Thread.isMainThread }
                held.signal()
            }

            func release() {}
        }
        let gate = Gate()
        let hold = GuestGPUHold(gate: gate)

        hold.arm(after: 0.05)
        // This test runs on the main thread, and the main thread waits here
        // until the hold lands, which it can only do from another thread.
        let landed = gate.held.wait(timeout: .now() + .seconds(5))

        #expect(landed == .success)
        #expect(gate.heldOnMainThread == false)
        #expect(hold.isHeld)
    }

    @MainActor
    @Test
    func aWithdrawnBoundNeverFiresOnTheProductionTimer() {
        let log = CallLog()
        let hold = GuestGPUHold(gate: RecordingGate(log))

        hold.arm(after: 0.05)
        hold.release()
        Thread.sleep(forTimeInterval: 0.3)

        #expect(log.entries.isEmpty)
        #expect(!hold.isHeld)
    }
}
