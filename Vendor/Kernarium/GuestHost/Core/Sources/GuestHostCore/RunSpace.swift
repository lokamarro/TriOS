// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Decides when a running guest is paused for want of space on the device.
///
/// A guest's disk is a sparse file that grows as the guest writes, and the
/// guest mounts it so that a write the device cannot take stops it (Android's
/// data disk is `errors=panic`). The start check covers only the first
/// writes, so while the guest runs the application reads the free space the
/// way it states it elsewhere and pauses the guest before the disk can fail
/// to grow. The runtime's suspension does the pausing (`SpacePauser`); this
/// type holds the numbers and the rules that follow from them.
public enum RunSpace {
    public enum Verdict: Equatable, Sendable {
        case keepRunning
        /// Free space fell below `pauseBelowBytes`.
        case pause
    }

    /// Free space any start needs for the guest's own writes.
    public static let startReserveBytes: UInt64 = Bytes.gib * 3 / 2
    /// Free space the first start of an Android guest needs, which writes its
    /// first setup into a data disk that does not exist yet.
    public static let firstStartReserveBytes: UInt64 = Bytes.gib * 3
    /// Free space below which a running guest is paused: what its storage
    /// may still grow by before it cannot, less what the pause itself
    /// writes. The write-back of a guest's dirty pages and the writes of one
    /// check interval come out of it.
    public static let pauseBelowBytes: UInt64 = Bytes.gib
    /// Free space a paused guest needs to run again: what a start needs, so
    /// the guest is not paused again straight away.
    public static let resumeAtBytes: UInt64 = startReserveBytes

    /// The writes one check interval is assumed to take, per second, when the
    /// next check is placed. A guest writes this fast only briefly, and a
    /// guess too high costs a few more checks, not a missed pause.
    public static let assumedWriteRate: UInt64 = 128 * Bytes.mib
    public static let shortestInterval = Duration.seconds(2)
    public static let longestInterval = Duration.seconds(30)

    /// What one reading of the free space says of a running guest.
    public static func verdict(freeBytes: UInt64) -> Verdict {
        freeBytes < pauseBelowBytes ? .pause : .keepRunning
    }

    /// Whether a paused guest may run again at this much free space. More is
    /// needed than the pause stopped it at, or the next write pauses it again.
    public static func allowsResume(freeBytes: UInt64) -> Bool {
        freeBytes >= resumeAtBytes
    }

    /// How long to wait before the next reading: the time the guest would
    /// take to write what separates `freeBytes` from the pause, at
    /// `assumedWriteRate`, so a nearly full device is read often and a roomy
    /// one rarely. Never less than `shortestInterval` or more than
    /// `longestInterval`.
    public static func nextCheck(freeBytes: UInt64) -> Duration {
        let margin = freeBytes > pauseBelowBytes ? freeBytes - pauseBelowBytes : 0
        let seconds = Double(margin) / Double(assumedWriteRate)
        return min(max(.seconds(seconds), shortestInterval), longestInterval)
    }
}
