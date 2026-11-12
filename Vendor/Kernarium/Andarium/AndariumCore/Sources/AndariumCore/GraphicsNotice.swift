// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// When the session tells the person that Android ran out of graphics memory.
///
/// The host gives the guest's graphics a fixed share of the device's memory.
/// When an allocation does not fit, even after the host has freed what it can
/// and tried again, the guest is refused, and an app that was drawing with it
/// (Chrome's WebGL is the one that shows it) loses its graphics until it is
/// restarted. The runtime counts the refusals the guest was answered with
/// (`gpu_memory_refusals`); this turns that count into one notice per burst.
///
/// A burst is refusals that follow each other less than `quietInterval`
/// apart: the first announces itself and the rest of the burst, dismissed or
/// not, announce nothing, so a page that fails over and over is one notice and
/// not a stream. A refusal after a quiet `quietInterval` starts a new burst.
/// The notice stays until the person dismisses it or `visibleFor` has passed.
public struct GraphicsNotice: Equatable, Sendable {
    /// How often the count is read while a guest runs.
    public static let pollInterval = Duration.seconds(2)
    /// How long without a refusal ends a burst.
    public static let quietInterval = Duration.seconds(60)
    /// How long a notice stays up when nobody dismisses it.
    public static let visibleFor = Duration.seconds(30)

    /// The words of the notice.
    public static let title = "Android ran out of graphics memory"
    public static let detail = "If an app looks broken, swipe it away in Recents and open it again."
    public static let dismissLabel = "Dismiss"

    /// A notice is up.
    public private(set) var isShowing = false

    private var count: UInt64?
    private var shownAt: ContinuousClock.Instant?
    private var lastRefusalAt: ContinuousClock.Instant?

    public init() {}

    /// A reading of the refusal count at `now`. The first reading is the
    /// baseline: what was refused before it is not announced. A count that
    /// went backwards belongs to a new display and is a new baseline too.
    /// Returns whether a notice appeared with this reading.
    @discardableResult
    public mutating func record(refusals: UInt64, at now: ContinuousClock.Instant) -> Bool {
        expire(at: now)
        defer { count = refusals }
        guard let before = count, refusals > before else { return false }
        let newBurst: Bool
        if let last = lastRefusalAt {
            newBurst = last.duration(to: now) >= Self.quietInterval
        } else {
            newBurst = true
        }
        lastRefusalAt = now
        guard newBurst else { return false }
        isShowing = true
        shownAt = now
        return true
    }

    /// Takes the notice away after `visibleFor`, if nobody has yet.
    public mutating func expire(at now: ContinuousClock.Instant) {
        guard isShowing, let shownAt, shownAt.duration(to: now) >= Self.visibleFor else { return }
        isShowing = false
        self.shownAt = nil
    }

    /// The person dismisses the notice. The burst it announced goes on
    /// without another.
    public mutating func dismiss() {
        isShowing = false
        shownAt = nil
    }
}
