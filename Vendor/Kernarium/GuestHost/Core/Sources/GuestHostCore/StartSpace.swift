// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// A start the device has too little free space for, as the screen says it.
/// The guest's disk grows as it writes, so a start needs room for the
/// guest's own first writes (`RunSpace.startReserveBytes`, more for the first
/// start of Android).
public struct StartSpace: Equatable, Sendable {
    public let neededBytes: UInt64
    public let freeBytes: UInt64

    public init(neededBytes: UInt64, freeBytes: UInt64) {
        self.neededBytes = neededBytes
        self.freeBytes = freeBytes
    }

    /// The shortage a start with `freeBytes` free has, or nil when it has room.
    public init?(freeBytes: UInt64, firstStart: Bool = false) {
        let needed = firstStart ? RunSpace.firstStartReserveBytes : RunSpace.startReserveBytes
        guard freeBytes < needed else { return nil }
        neededBytes = needed
        self.freeBytes = freeBytes
    }

    public static let title = "Not enough free space"

    /// What is the matter, in the figures the device has now; `device` is
    /// what the screen calls it: "iPad", "iPhone".
    public func message(device: String) -> String {
        "Starting needs \(Formats.size(bytes: neededBytes)) free for the guest's own writes. "
            + "This \(device) has \(Formats.size(bytes: freeBytes)) free."
    }

    /// The way on, where the screen cannot say it any other way.
    public func advice(device: String) -> String {
        "Free up space in Settings \u{203A} General \u{203A} \(device) Storage."
    }
}
