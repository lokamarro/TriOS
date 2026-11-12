// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the iPhone, iPad or Mac running Andarium offers.
public struct HostDevice: Equatable, Sendable {
    public var name: String
    /// The vCPUs a guest may have, and the count a new guest takes:
    /// `min(8, physical cores)`, every core of the device at most the count the
    /// memory budget is measured for. The application states it from the
    /// loader (`GuestProcessorLimit`), so it is also the number of rows
    /// `memory` has and of positions the processor ruler offers.
    public var processorCount: Int
    public var memory: MemoryPolicy
    public var freeStorageBytes: UInt64
    /// Whether the device's GPU can draw the guest. The guest's graphics need
    /// Metal 4, which the A14 and later chips (and the M1 and later) have.
    public var runsGuestGraphics: Bool

    /// `processorCount` is the vCPU limit.
    public init(
        name: String, processorCount: Int, memory: MemoryPolicy, freeStorageBytes: UInt64,
        runsGuestGraphics: Bool = true
    ) {
        self.name = name
        self.processorCount = max(1, processorCount)
        self.memory = memory
        self.freeStorageBytes = freeStorageBytes
        self.runsGuestGraphics = runsGuestGraphics
    }

    public func hasRoom(for bytes: UInt64) -> Bool { freeStorageBytes >= bytes }
}

/// What Home says on a device that cannot draw the guest. The requirement is
/// the chip's, so the words name the chip and leave the rest of the device out.
public enum DeviceRequirement {
    public static let title = "Needs an A14 chip or later"

    public static func explanation(deviceName: String) -> String {
        "Andarium draws the guest with Metal 4, which this \(deviceName) does not have, so the guest cannot start here."
    }
}
