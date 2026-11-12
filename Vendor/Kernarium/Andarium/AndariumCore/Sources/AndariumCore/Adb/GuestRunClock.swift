// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// A clock that counts only the time the guest runs. The application pauses it
/// while `PauseController` holds the guest paused, so a timeout on an adb
/// operation never fires because Andarium spent minutes in the background: the
/// guest could not have answered meanwhile.
public final class GuestRunClock: @unchecked Sendable {
    private let lock = NSLock()
    private let source: @Sendable () -> Duration
    private var accumulated = Duration.zero
    /// The source's reading when the clock last resumed; nil while paused.
    private var resumedAt: Duration?

    /// `source` is a monotonic reading; the default is the system's continuous
    /// clock, which also counts the time the device sleeps.
    public init(source: @escaping @Sendable () -> Duration = GuestRunClock.continuousNow) {
        self.source = source
        resumedAt = source()
    }

    public static let continuousNow: @Sendable () -> Duration = {
        ContinuousClock.now - GuestRunClock.origin
    }

    private static let origin = ContinuousClock.now

    /// Running time since the clock was made, without the paused spans.
    public var now: Duration {
        lock.withLock {
            guard let resumedAt else { return accumulated }
            return accumulated + (source() - resumedAt)
        }
    }

    public var isPaused: Bool {
        lock.withLock { resumedAt == nil }
    }

    /// Stops counting. Pausing a paused clock changes nothing.
    public func pause() {
        lock.withLock {
            guard let resumedAt else { return }
            accumulated += source() - resumedAt
            self.resumedAt = nil
        }
    }

    /// Counts again from now. Resuming a running clock changes nothing.
    public func resume() {
        lock.withLock {
            guard resumedAt == nil else { return }
            resumedAt = source()
        }
    }
}
