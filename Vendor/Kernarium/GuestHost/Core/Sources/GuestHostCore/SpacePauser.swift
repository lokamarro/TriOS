// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What pausing a guest for space does to the runtime, behind one seam so the
/// logic is tested without it. The runtime calls run on a serial queue the
/// application owns, in the order asked.
@MainActor
public protocol SpacePauseEffects: AnyObject {
    /// Resumes a quiesced guest behind any quiesce still in flight and calls
    /// `finished` on the main actor once it is back.
    func resumeGuest(finished: @escaping @MainActor () -> Void)
    /// Writes the guest's storage back and suspends it like a quiesce, but
    /// keeps its RAM in the process and writes no image of it, and calls
    /// `finished` on the main actor once the runtime has returned: for a host
    /// short of the space an image takes.
    func suspendGuestResident(reason: String, finished: @escaping @MainActor () -> Void)
    /// The processor time the guest's vCPU threads have used in this boot, in
    /// nanoseconds; nil when it cannot be read. A suspended guest uses none.
    func guestProcessorTime() async -> UInt64?
}

/// A guest paused for want of space on the device: suspended with the
/// runtime's own suspension, confirmed by its processor time standing still,
/// and held so until the person resumes it. What decides to pause the guest
/// (`RunSpace`) and what the lifecycle edges do to it belong to the
/// application's own controller, which asks `isPaused` before it touches a
/// guest.
@MainActor
public final class SpacePauser {
    /// Attempts at suspending the guest: the first, and one more behind a wake
    /// when the guest did not hold still.
    public static let suspendAttempts = 2

    /// The guest counts as paused from `begin` until it is resumed, its run
    /// ends, or every attempt to suspend it failed.
    public private(set) var isPaused = false
    private let effects: any SpacePauseEffects
    private let wait: (Duration) async -> Void

    /// `wait` is how the confirmation waits between its readings: real time
    /// unless a test says otherwise.
    public init(
        effects: any SpacePauseEffects,
        wait: @escaping (Duration) async -> Void = { try? await Task.sleep(for: $0) }
    ) {
        self.effects = effects
        self.wait = wait
    }

    /// The guest counts as paused from now: nothing but `resume` may wake it.
    public func begin() {
        isPaused = true
    }

    /// The guest's run ended: nothing is left to resume.
    public func end() {
        isPaused = false
    }

    /// Suspends the guest of a begun pause with the runtime's suspension,
    /// keeping its RAM in the process, because the image of a hibernation is
    /// what the device is short of. The suspension is confirmed by the guest's
    /// processor time standing still (`SuspensionCheck`), and a guest that
    /// does not hold still is woken and suspended once more.
    ///
    /// Returns `.held` once it holds still. After the last attempt a guest
    /// that still runs is left running and no longer counts as paused, and
    /// this returns `.running`. Returns nil when the pause ended meanwhile.
    public func settle() async -> SpaceSuspension? {
        guard isPaused else { return nil }
        for attempt in 0..<Self.suspendAttempts {
            if attempt > 0 {
                await wake()
                guard isPaused else { return nil }
            }
            await suspend()
            guard isPaused else { return nil }
            let verdict = await SuspensionCheck.confirm(
                reading: { await self.effects.guestProcessorTime() },
                wait: wait)
            guard isPaused, !Task.isCancelled else { return nil }
            if verdict == .held { return .held }
        }
        isPaused = false
        return .running
    }

    /// The person resumes the guest. `finished` runs on the main actor once it
    /// is back. Returns whether a resume began: not when no pause is held,
    /// and `finished` does not run then.
    @discardableResult
    public func resume(finished: @escaping @MainActor () -> Void) -> Bool {
        guard isPaused else { return false }
        isPaused = false
        effects.resumeGuest(finished: finished)
        return true
    }

    private func suspend() async {
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            effects.suspendGuestResident(reason: "low space") { continuation.resume() }
        }
    }

    private func wake() async {
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            effects.resumeGuest { continuation.resume() }
        }
    }
}
