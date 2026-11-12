// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The guest's GPU request gate as the background policy drives it. The runtime
/// serializes the two calls, which may be made from any thread.
nonisolated protocol GuestGPUGate: Sendable {
    /// Closes the gate: no guest request reaches the host GPU afterwards.
    func hold()
    func release()
}

/// Runs an action once, after a delay, on a thread that is not the main
/// thread, and returns what withdraws it.
typealias GuestGPUHoldTimer =
    @Sendable (TimeInterval, @escaping @Sendable () -> Void) -> @Sendable () -> Void

/// The hold on the guest's GPU requests, which more than one path can take and
/// none may delay.
///
/// iOS revokes the host GPU a few seconds after the app enters the background,
/// and a guest request that reaches the GPU then is rejected: the Vulkan device
/// survives only the rejections the driver can see. The hold therefore has a
/// bound that is a timer of its own: it runs off the main thread, so a main
/// thread that is waiting for the guest's storage to settle, or for its screen
/// to go off, cannot hold the hold back. Whichever comes first, the path that
/// holds at once or the bound, takes it; the other finds it taken.
nonisolated final class GuestGPUHold: @unchecked Sendable {
    private let gate: any GuestGPUGate
    private let timer: GuestGPUHoldTimer
    private let lock = NSLock()
    private var held = false
    /// Which bound is armed: a bound that fires as it is withdrawn or replaced
    /// sees another number and does nothing.
    private var generation = 0
    private var withdrawBound: (@Sendable () -> Void)?

    init(
        gate: any GuestGPUGate,
        timer: @escaping GuestGPUHoldTimer = GuestGPUHold.dispatchTimer
    ) {
        self.gate = gate
        self.timer = timer
    }

    var isHeld: Bool { lock.withLock { held } }

    /// Holds the GPU now, and withdraws the bound that would have. Returns
    /// whether this call took the hold, which is not so when it was taken.
    ///
    /// The gate may wait for requests already inside the device, so a caller
    /// that arrives while another is holding waits for it.
    @discardableResult
    func hold() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        withdrawLocked()
        return takeLocked()
    }

    /// Holds the GPU in `seconds` unless something held or released it first,
    /// on a thread of its own. `landed` runs on that thread once the bound has
    /// taken the hold.
    func arm(
        after seconds: TimeInterval,
        landed: (@Sendable () -> Void)? = nil
    ) {
        lock.lock()
        defer { lock.unlock() }
        withdrawLocked()
        let mine = generation
        withdrawBound = timer(max(seconds, 0)) { [weak self] in
            self?.bound(mine, landed)
        }
    }

    /// Withdraws the bound and releases the hold if it is taken. Returns
    /// whether it released one.
    @discardableResult
    func release() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        withdrawLocked()
        guard held else { return false }
        gate.release()
        held = false
        return true
    }

    private func bound(_ armed: Int, _ landed: (@Sendable () -> Void)?) {
        lock.lock()
        guard generation == armed else {
            lock.unlock()
            return
        }
        withdrawBound = nil
        generation += 1
        let took = takeLocked()
        lock.unlock()
        if took { landed?() }
    }

    private func takeLocked() -> Bool {
        guard !held else { return false }
        held = true
        gate.hold()
        return true
    }

    private func withdrawLocked() {
        generation += 1
        withdrawBound?()
        withdrawBound = nil
    }

    /// A dispatch timer on a queue of its own, at the top quality of service:
    /// the hold is what keeps the guest off a GPU that iOS is about to revoke.
    static let dispatchTimer: GuestGPUHoldTimer = { seconds, action in
        let source = DispatchSource.makeTimerSource(
            queue: DispatchQueue(
                label: "com.imxieyi.Kernarium.gpu-hold",
                qos: .userInteractive
            )
        )
        source.schedule(
            deadline: .now() + seconds,
            leeway: .milliseconds(1)
        )
        let handle = source
        source.setEventHandler {
            action()
            handle.cancel()
        }
        source.resume()
        return { handle.cancel() }
    }
}
