// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// One reading of the running guest's host-side figures, as the runtime
/// reports them (`kernarium_runtime_statistics`). Two readings of one boot
/// give rates; a reading alone gives none.
public struct StatsSample: Equatable, Sendable {
    /// How many times this launch's guest has restarted: 0 for the first
    /// boot. Two readings with different values span two guests.
    public var bootIndex: UInt32
    /// The guest's vCPUs that have come up.
    public var processorCount: UInt32
    /// When the CPU time was read, in nanoseconds of the steady clock.
    public var timestampNanoseconds: UInt64
    /// The user and system time the guest's vCPU threads have used in this
    /// boot, in nanoseconds.
    public var processorTimeNanoseconds: UInt64
    /// Frames that reached the screen, since the display was made.
    public var presentedFrames: UInt64
    /// The guest's RAM, and the part of it the iPad holds now.
    public var ramBytes: UInt64
    public var ramHeldBytes: UInt64
    /// The host GPU's memory budget for the guest and what the process holds
    /// against it; both 0 while there is no budget.
    public var gpuMemoryBudgetBytes: UInt64
    public var gpuMemoryUsedBytes: UInt64

    public init(
        bootIndex: UInt32, processorCount: UInt32, timestampNanoseconds: UInt64,
        processorTimeNanoseconds: UInt64, presentedFrames: UInt64, ramBytes: UInt64 = 0,
        ramHeldBytes: UInt64 = 0, gpuMemoryBudgetBytes: UInt64 = 0, gpuMemoryUsedBytes: UInt64 = 0
    ) {
        self.bootIndex = bootIndex
        self.processorCount = processorCount
        self.timestampNanoseconds = timestampNanoseconds
        self.processorTimeNanoseconds = processorTimeNanoseconds
        self.presentedFrames = presentedFrames
        self.ramBytes = ramBytes
        self.ramHeldBytes = ramHeldBytes
        self.gpuMemoryBudgetBytes = gpuMemoryBudgetBytes
        self.gpuMemoryUsedBytes = gpuMemoryUsedBytes
    }
}

/// The guest kernel's own memory figures, which it answers on request.
public struct GuestMemorySample: Equatable, Sendable {
    public var totalBytes: UInt64
    public var availableBytes: UInt64

    public init(totalBytes: UInt64, availableBytes: UInt64) {
        self.totalBytes = totalBytes
        self.availableBytes = availableBytes
    }

    /// What Android and `free` call in use: the total less what is available
    /// without reclaiming anything that matters, so the page cache it can drop
    /// is not counted.
    public var inUseBytes: UInt64 {
        totalBytes > availableBytes ? totalBytes - availableBytes : 0
    }
}

/// Turns the runtime's readings into the figures the screens show.
///
/// The CPU share is the part of the guest's vCPUs that computed between two
/// readings, 0 to 100, and the frame rate the frames that reached the screen
/// per second. Both come from two readings of one boot; until there are two,
/// and after a pause, they are `nil` and a screen shows a dash. A reading
/// whose counters or time went backwards, or whose vCPU count changed, starts
/// a new baseline instead of a rate. A reading of another boot discards what
/// the last guest showed.
public struct StatsMeter: Equatable, Sendable {
    /// How many CPU values the history keeps.
    public static let historyLength = 10

    public private(set) var cpuPercent: Int?
    public private(set) var framesPerSecond: Int?
    /// The last CPU values, oldest first.
    public private(set) var history: [Int] = []
    /// The guest's own memory in use. A request the guest could not answer
    /// leaves the last value.
    public private(set) var memoryInUseBytes: UInt64?
    /// The guest RAM the iPad holds, from the last reading.
    public private(set) var ramHeldBytes: UInt64?
    /// The graphics memory in use against the budget, and the budget, from
    /// the last reading; nil while there is no budget.
    public private(set) var gpuMemoryUsedBytes: UInt64?
    public private(set) var gpuMemoryBudgetBytes: UInt64?
    /// The boot the figures belong to.
    public private(set) var bootIndex: UInt32?
    private var baseline: StatsSample?

    public init() {}

    /// The next reading starts a new baseline: no rate spans the pause.
    public mutating func pause() {
        baseline = nil
    }

    public mutating func record(_ sample: StatsSample) {
        if let bootIndex, bootIndex != sample.bootIndex { self = StatsMeter() }
        bootIndex = sample.bootIndex
        ramHeldBytes = sample.ramBytes == 0 ? nil : sample.ramHeldBytes
        let budgeted = sample.gpuMemoryBudgetBytes > 0
        gpuMemoryBudgetBytes = budgeted ? sample.gpuMemoryBudgetBytes : nil
        gpuMemoryUsedBytes = budgeted ? sample.gpuMemoryUsedBytes : nil
        defer { baseline = sample }
        guard let before = baseline else { return }
        guard sample.timestampNanoseconds > before.timestampNanoseconds,
            sample.processorTimeNanoseconds >= before.processorTimeNanoseconds,
            sample.presentedFrames >= before.presentedFrames,
            sample.processorCount == before.processorCount, sample.processorCount > 0
        else { return }
        let seconds = Double(sample.timestampNanoseconds - before.timestampNanoseconds) / 1e9
        let busy = Double(sample.processorTimeNanoseconds - before.processorTimeNanoseconds) / 1e9
        let share = busy / (seconds * Double(sample.processorCount)) * 100
        let cpu = Int(min(max(share, 0), 100).rounded())
        let frames = Double(sample.presentedFrames - before.presentedFrames) / seconds
        cpuPercent = cpu
        framesPerSecond = Int(min(frames, 9999).rounded())
        history = Array((history + [cpu]).suffix(Self.historyLength))
    }

    public mutating func record(memory: GuestMemorySample) {
        // A kernel that reports no total has not answered.
        guard memory.totalBytes > 0 else { return }
        memoryInUseBytes = memory.inUseBytes
    }
}

/// When the statistics are read, and how often. Nothing is read unless a view
/// shows them.
public enum StatsSampling {
    /// A run of readings starts with two this far apart, so the figures show
    /// within about a third of a second.
    public static let firstGap = Duration.milliseconds(250)
    /// Then the readings go on at this interval, and the guest's memory is
    /// asked for with every `memoryEvery`th.
    public static let interval = Duration.seconds(1)
    public static let memoryEvery = 2

    /// Whether the readings run: some view shows them, the application is
    /// active, the runtime can read them, there is a runtime to ask and the
    /// guest runs.
    public static func isWanted(
        viewers: Int, applicationActive: Bool, capable: Bool, hasDriver: Bool, running: Bool
    ) -> Bool {
        viewers > 0 && applicationActive && capable && hasDriver && running
    }
}
