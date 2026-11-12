// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What guest memory the host can honour, at each vCPU count. Every figure is
/// the runtime's own (`runtime/guest_memory_budget.hpp`), which the application
/// reads through the loader the way Kernarium does, and none is computed here:
///
/// - the bound is the most a person may choose: the largest multiple of 64 MiB
///   in 512...4096 that leaves the host GPU its floor under the memory limit
///   the system sets this process;
/// - the safe size is the line above which iOS is likely to end the
///   application: the same measure with the launch's conservative allowances
///   for the application, the rest of the engine and headroom, in 0.5 GiB
///   steps and within 1.5 to 4 GiB, and never above the bound;
/// - the default is what a new guest gets: the safe size, and on a device
///   whose bound is the top of the range also the most that leaves the host
///   GPU a gibibyte, so it is never above the safe size;
/// - the graphics memory is the host GPU's budget, B, a guest of a size and
///   vCPU count would launch with.
///
/// Each added vCPU holds more of the process's memory, so neither bound,
/// safe size nor default rises with the count. The limit is fixed for the
/// process, so they are read once, for every count the host has.
///
/// The least a person may choose is `Limits.minimumMemoryMiB`, or the bound
/// where a device's limit leaves less than that: the bound always wins, so the
/// picker is never empty and never offers what the host cannot honour.
public struct MemoryPolicy: Equatable, Sendable {
    /// The most guest RAM at 1, 2, ... vCPUs, each on the runtime's steps and
    /// inside its range.
    private let maximums: [Int]
    /// What a new guest gets at the same counts, on the same steps.
    private let defaults: [Int]
    /// The line above which iOS is likely to end the application, at the same
    /// counts: no lower than the default, no higher than the bound.
    private let safes: [Int]
    /// The graphics memory at each count (one row each) and each of the
    /// runtime's memory steps from the bottom of its range; empty where the
    /// host does not say.
    private let graphics: [[Int]]

    /// The bounds, the defaults and the safe sizes as they are: each is taken
    /// down to the runtime's steps and into its range. A count without a
    /// default takes its bound, and one without a safe size its default.
    /// `graphicsMiB` answers the graphics memory of a size at a count, which
    /// is asked once for every size of every count.
    public init(
        maximumsMiB: [Int], defaultsMiB: [Int], safeMiB: [Int] = [],
        graphicsMiB: ((_ memoryMiB: Int, _ vCPUs: Int) -> Int)? = nil
    ) {
        let values = maximumsMiB.isEmpty ? [Limits.runtimeMemoryRangeMiB.upperBound] : maximumsMiB
        let bounds = values.map { Self.steppedDown($0) }
        maximums = bounds
        let steppedDefaults = bounds.indices.map { index in
            index < defaultsMiB.count ? Self.steppedDown(defaultsMiB[index]) : bounds[index]
        }
        defaults = steppedDefaults
        safes = bounds.indices.map { index in
            guard index < safeMiB.count else { return steppedDefaults[index] }
            return min(max(Self.steppedDown(safeMiB[index]), steppedDefaults[index]), bounds[index])
        }
        if let graphicsMiB {
            graphics = bounds.indices.map { index in
                Self.runtimeSteps.map { graphicsMiB($0, index + 1) }
            }
        } else {
            graphics = []
        }
    }

    /// The bounds alone: every default and safe size is its bound, so nothing
    /// is above them. A host that sets no limit, and the fixtures that do not
    /// exercise the default.
    public init(maximumsMiB: [Int]) {
        self.init(maximumsMiB: maximumsMiB, defaultsMiB: [])
    }

    /// The bounds of 1...`processorCount` vCPUs from `bound`, the loader's
    /// answer for a count; every default is its bound.
    public init(processorCount: Int, bound: (Int) -> Int) {
        self.init(maximumsMiB: (1...max(processorCount, 1)).map(bound))
    }

    /// The bounds, defaults, safe sizes and graphics memory of
    /// 1...`processorCount` vCPUs from the loader's answers for a count: a
    /// safe size defaults to the default, and without `graphicsMemory` the
    /// host gives no estimate.
    public init(
        processorCount: Int, bound: (Int) -> Int, defaultMemory: (Int) -> Int,
        safeMemory: ((Int) -> Int)? = nil,
        graphicsMemory: ((_ memoryMiB: Int, _ vCPUs: Int) -> Int)? = nil
    ) {
        let counts = 1...max(processorCount, 1)
        self.init(
            maximumsMiB: counts.map(bound), defaultsMiB: counts.map(defaultMemory),
            safeMiB: safeMemory.map { rule in counts.map(rule) } ?? [], graphicsMiB: graphicsMemory
        )
    }

    /// The most guest RAM the picker offers at `vCPUs` vCPUs; a count above the
    /// host's takes the last bound.
    public func maximumMiB(vCPUs: Int) -> Int {
        maximums[min(max(vCPUs, 1), maximums.count) - 1]
    }

    /// The least guest RAM the picker offers at `vCPUs` vCPUs: the minimum
    /// for Android, or the bound where that is lower.
    public func minimumMiB(vCPUs: Int) -> Int {
        min(Limits.minimumMemoryMiB, maximumMiB(vCPUs: vCPUs))
    }

    /// Every position of the picker at `vCPUs` vCPUs: the minimum up to the
    /// bound, in the runtime's 64 MiB steps.
    public func stops(vCPUs: Int) -> [Int] {
        Array(stride(
            from: minimumMiB(vCPUs: vCPUs), through: maximumMiB(vCPUs: vCPUs), by: Limits.memoryStepMiB
        ))
    }

    /// What a new guest gets at `vCPUs` vCPUs; a count above the host's takes
    /// the last one.
    public func defaultMemoryMiB(vCPUs: Int) -> Int {
        defaults[min(max(vCPUs, 1), defaults.count) - 1]
    }

    /// The size above which iOS is likely to end the application at `vCPUs`
    /// vCPUs; a count above the host's takes the last one. It is the default
    /// unless the default is held lower for the graphics memory.
    public func safeMemoryMiB(vCPUs: Int) -> Int {
        safes[min(max(vCPUs, 1), safes.count) - 1]
    }

    /// Whether a guest with `mib` of memory and `vCPUs` vCPUs has more than a
    /// new guest gets: graphics-hungry apps are then likely to run out of
    /// graphics memory, though iOS need not end the application.
    public func isAboveDefault(_ mib: Int, vCPUs: Int) -> Bool {
        mib > defaultMemoryMiB(vCPUs: vCPUs)
    }

    /// Whether a guest with `mib` of memory and `vCPUs` vCPUs has more than
    /// `safeMemoryMiB`: iOS is then likely to end the application while the
    /// guest runs. The same figure on Home and on the memory picker, so they
    /// agree at every size and count.
    public func isAboveSafe(_ mib: Int, vCPUs: Int) -> Bool {
        mib > safeMemoryMiB(vCPUs: vCPUs)
    }

    /// What a guest of `mib` of memory and `vCPUs` vCPUs risks, the worse of
    /// the two when it risks both: iOS ending the application, then the guest
    /// running out of graphics memory.
    public func risk(_ mib: Int, vCPUs: Int) -> MemoryRisk {
        if isAboveSafe(mib, vCPUs: vCPUs) { return .closing }
        return isAboveDefault(mib, vCPUs: vCPUs) ? .graphics : .none
    }

    /// The graphics memory, in MiB, a guest of `mib` of memory and `vCPUs`
    /// vCPUs would launch with: the host GPU's budget, from the runtime's own
    /// formula. A size is taken to the runtime's step below it, and a count
    /// above the host's takes the last one. Nil where the host does not say.
    public func graphicsMemoryMiB(forMemory mib: Int, vCPUs: Int) -> Int? {
        guard !graphics.isEmpty else { return nil }
        let row = graphics[min(max(vCPUs, 1), graphics.count) - 1]
        let range = Limits.runtimeMemoryRangeMiB
        let offset = min(max(mib, range.lowerBound), range.upperBound) - range.lowerBound
        return row[offset / Limits.runtimeMemoryStepMiB]
    }

    /// A stored or requested size at `vCPUs` vCPUs: on the nearest step, at
    /// least the minimum and no more than the bound.
    public func clamped(_ mib: Int, vCPUs: Int) -> Int {
        min(max(Self.nearestStep(mib), Limits.minimumMemoryMiB), maximumMiB(vCPUs: vCPUs))
    }

    /// Whether the runtime accepts `mib` (a value of its own step in range).
    public static func runtimeAccepts(_ mib: Int) -> Bool {
        Limits.runtimeMemoryRangeMiB.contains(mib)
            && (mib - Limits.runtimeMemoryRangeMiB.lowerBound) % Limits.runtimeMemoryStepMiB == 0
    }

    /// The sizes the runtime accepts, from the bottom of its range.
    private static var runtimeSteps: [Int] {
        Array(stride(
            from: Limits.runtimeMemoryRangeMiB.lowerBound, through: Limits.runtimeMemoryRangeMiB.upperBound,
            by: Limits.runtimeMemoryStepMiB
        ))
    }

    private static func nearestStep(_ mib: Int) -> Int {
        let range = Limits.runtimeMemoryRangeMiB
        let offset = min(max(mib, range.lowerBound), range.upperBound) - range.lowerBound
        let step = Limits.runtimeMemoryStepMiB
        return range.lowerBound + (offset + step / 2) / step * step
    }

    private static func steppedDown(_ mib: Int) -> Int {
        let range = Limits.runtimeMemoryRangeMiB
        let offset = min(max(mib, range.lowerBound), range.upperBound) - range.lowerBound
        return range.lowerBound + offset / Limits.runtimeMemoryStepMiB * Limits.runtimeMemoryStepMiB
    }
}

/// What a guest of some size risks: the application being ended by iOS
/// (`MemoryWarning`), or the guest running out of graphics memory
/// (`GraphicsMemoryWarning`).
public enum MemoryRisk: Equatable, Sendable {
    case none
    case graphics
    case closing
}

/// What the application says when a guest has more memory than `safeMemoryMiB`
/// (`MemoryPolicy.isAboveSafe`): iOS is then likely to close Andarium while
/// the guest runs. Home and the memory picker use these words, so they say the
/// same thing.
public enum MemoryWarning {
    public static let title = "Guest memory is high"

    /// Home's notice: the guest's size, the size that is safer, and where to
    /// lower it.
    public static func explanation(memoryMiB: Int, safeMiB: Int, deviceName: String) -> String {
        "iOS is likely to close Andarium with \(Formats.size(mib: memoryMiB)) of guest memory on this "
            + "\(deviceName). \(Formats.size(mib: safeMiB)) or less is safer. "
            + "Lower it with Edit under Guest system."
    }

    /// The line beneath the memory picker.
    public static func note(safeMiB: Int) -> String {
        "iOS is likely to close Andarium above \(Formats.size(mib: safeMiB))."
    }
}

/// What the System sheet says of the graphics memory (VRAM): that it is not
/// part of the guest's RAM, and, when a guest has more RAM than a new one gets
/// (`MemoryPolicy.isAboveDefault`), that a game may run short of it.
public enum GraphicsMemoryWarning {
    public static let title = "VRAM"

    /// Beneath the estimate: what VRAM is relative to the memory above it.
    public static let separateNote = "VRAM is graphics memory, separate from the guest\u{2019}s RAM."

    /// The line beneath the memory picker above the default and not above the
    /// safe size.
    public static func note(defaultMiB: Int) -> String {
        "Above \(Formats.size(mib: defaultMiB)), games may run out of graphics memory."
    }
}
