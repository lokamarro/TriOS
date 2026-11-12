// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What a guest paused for space came to: suspended, as its processor time
/// standing still shows, or still running after every attempt.
public enum SpaceSuspension: Equatable, Sendable {
    case held
    case running
}

/// Whether a guest the runtime says it suspended really holds still. The
/// runtime cannot say whether the guest confirmed its suspension, and a guest
/// that did not goes on running and writing behind a screen that calls it
/// paused, so the processor time its vCPU threads use is read until it stands
/// still, within a bound.
public enum SuspensionCheck {
    public enum Verdict: Equatable, Sendable {
        case held
        case running
        /// The reading is not available, or the wait was cancelled.
        case unknown
    }

    public static let interval = Duration.seconds(1)
    /// The processor time a guest may use over one interval and still count as
    /// held: a suspended guest's threads are held and use none, and the bound
    /// only leaves room for the clock's own rounding.
    public static let stillNanoseconds: UInt64 = 1_000_000
    /// Consecutive intervals that must pass still: one could be a guest idle
    /// between its timer ticks.
    public static let stillIntervals = 2
    /// The most readings, the first included, before the guest counts as
    /// running.
    public static let maximumReadings = 8

    @MainActor
    public static func confirm(
        reading: () async -> UInt64?, wait: (Duration) async -> Void
    ) async -> Verdict {
        guard var last = await reading() else { return .unknown }
        var still = 0
        for _ in 1..<maximumReadings {
            await wait(interval)
            guard !Task.isCancelled, let now = await reading() else { return .unknown }
            // A counter that went back belongs to another boot: it runs.
            let used = now >= last ? now - last : UInt64.max
            still = used <= stillNanoseconds ? still + 1 : 0
            if still >= stillIntervals { return .held }
            last = now
        }
        return .running
    }
}
