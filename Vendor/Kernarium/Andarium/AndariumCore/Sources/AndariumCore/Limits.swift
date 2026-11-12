// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

// What both applications decide the same way (sizes, free-space thresholds)
// reads as part of this module to the application and its tests.
@_exported import GuestHostCore

/// The ranges, defaults and reserves the application offers.
public enum Limits {
    /// A guest's storage capacity, in whole gibibytes, which the rulers offer
    /// in steps of `capacityStepGiB`. Storage only grows.
    public static let capacityRangeGiB = 4...128
    public static let capacityStepGiB = 4
    public static let defaultCapacityGiB = 8

    /// The sizes the storage rulers offer: 4, 8, 12 GiB and on in steps of
    /// `capacityStepGiB`, through the largest the system the application
    /// carries accepts (its data disk's maximum), and never above the range.
    public static func capacityStopsGiB(releaseMaximumGiB: Int = capacityRangeGiB.upperBound) -> [Int] {
        let top = min(releaseMaximumGiB, capacityRangeGiB.upperBound)
        guard top >= capacityRangeGiB.lowerBound else { return [capacityRangeGiB.lowerBound] }
        return Array(stride(from: capacityRangeGiB.lowerBound, through: top, by: capacityStepGiB))
    }

    /// What the runtime accepts for a guest's memory, in steps of 64 MiB. It
    /// is the runtime Kernarium's guests use too, so the range is the
    /// runtime's and Andarium narrows it only below.
    public static let runtimeMemoryRangeMiB = 512...4096
    public static let runtimeMemoryStepMiB = 64
    /// The least memory the application offers Android, the size below which
    /// Android is not usable. The memory ruler starts here and counts in
    /// `memoryStepMiB`; its top at a vCPU count comes from `MemoryPolicy`,
    /// which also takes the bottom down to that top on a device that cannot
    /// give this much.
    public static let minimumMemoryMiB = 1536
    public static let memoryStepMiB = runtimeMemoryStepMiB
    /// The range a new guest's memory is held within, in half gibibytes:
    /// `MemoryPolicy.defaultMemoryMiB` states the figure, from the runtime's
    /// own rule. A device whose limit leaves less than the bottom of it takes
    /// the bound of its vCPU count instead.
    public static let defaultMemoryRangeMiB = 1536...4096

    /// Free space a new guest needs for Android's first start, on top of the
    /// space its storage takes.
    public static let firstStartReserveBytes: UInt64 = RunSpace.firstStartReserveBytes
    /// Free space any start needs for Android's own writes; it is what a
    /// guest paused for space needs to resume (`RunSpace`).
    public static let startReserveBytes: UInt64 = RunSpace.startReserveBytes

    public static let bytesPerMiB: UInt64 = Bytes.mib
    public static let bytesPerGiB: UInt64 = Bytes.gib
}
