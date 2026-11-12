// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

private let second: UInt64 = 1_000_000_000

/// A reading of boot `boot` at `seconds`, with the vCPUs having computed
/// `busy` seconds in all since the boot began and `frames` reaching the
/// screen.
private func reading(
    boot: UInt32 = 0, cpus: UInt32 = 4, at seconds: Double, busy: Double = 0, frames: UInt64 = 0
) -> StatsSample {
    StatsSample(
        bootIndex: boot, processorCount: cpus,
        timestampNanoseconds: UInt64(seconds * Double(second)),
        processorTimeNanoseconds: UInt64(busy * Double(second)), presentedFrames: frames,
        ramBytes: 2048 << 20, ramHeldBytes: 1800 << 20
    )
}

struct StatsMeterTests {
    @Test func showsNothingBeforeTwoReadings() {
        var meter = StatsMeter()
        #expect(meter.cpuPercent == nil && meter.framesPerSecond == nil && meter.memoryInUseBytes == nil)
        meter.record(reading(at: 10))
        #expect(meter.cpuPercent == nil && meter.framesPerSecond == nil)
        #expect(meter.history.isEmpty)
    }

    @Test func theCPUShareIsTheBusyTimeOverTheVCPUsAndTheInterval() {
        var meter = StatsMeter()
        meter.record(reading(at: 10, busy: 5))
        // 1 s on 4 vCPUs, 1 s of CPU time among them: 25%.
        meter.record(reading(at: 11, busy: 6))
        #expect(meter.cpuPercent == 25)
        // 2 s, 8 s of CPU time on 4 vCPUs: all of them.
        meter.record(reading(at: 13, busy: 14))
        #expect(meter.cpuPercent == 100)
        #expect(meter.history == [25, 100])
    }

    @Test func theShareRoundsAndStaysWithinZeroAndAHundred() {
        var meter = StatsMeter()
        meter.record(reading(cpus: 2, at: 0, busy: 0))
        meter.record(reading(cpus: 2, at: 1, busy: 0.0049))
        #expect(meter.cpuPercent == 0)
        meter.record(reading(cpus: 2, at: 2, busy: 0.0049 + 0.345))
        #expect(meter.cpuPercent == 17)
        // Time the host charged beyond the interval cannot show more than all.
        meter.record(reading(cpus: 2, at: 3, busy: 0.3499 + 5))
        #expect(meter.cpuPercent == 100)
    }

    @Test func theFrameRateIsThePresentedFramesPerSecond() {
        var meter = StatsMeter()
        meter.record(reading(at: 0, frames: 100))
        meter.record(reading(at: 0.25, frames: 115))
        #expect(meter.framesPerSecond == 60)
        meter.record(reading(at: 1.25, frames: 115))
        #expect(meter.framesPerSecond == 0)
        meter.record(reading(at: 2.25, frames: 138))
        #expect(meter.framesPerSecond == 23)
    }

    @Test func theHistoryKeepsTheLastTen() {
        var meter = StatsMeter()
        meter.record(reading(cpus: 1, at: 0))
        for step in 1...14 {
            meter.record(reading(cpus: 1, at: Double(step), busy: Double(step) * Double(step) / 200))
        }
        #expect(meter.history.count == StatsMeter.historyLength)
        #expect(meter.history.last == meter.cpuPercent)
        #expect(meter.history == meter.history.sorted())
    }

    @Test func aPauseStartsANewBaseline() {
        var meter = StatsMeter()
        meter.record(reading(at: 0, busy: 0, frames: 0))
        meter.record(reading(at: 1, busy: 2, frames: 60))
        #expect(meter.cpuPercent == 50)
        meter.pause()
        // Ten seconds later: no rate spans the pause, and the last figures stay.
        meter.record(reading(at: 11, busy: 40, frames: 600))
        #expect(meter.cpuPercent == 50 && meter.framesPerSecond == 60)
        meter.record(reading(at: 12, busy: 41, frames: 600))
        #expect(meter.cpuPercent == 25 && meter.framesPerSecond == 0)
    }

    @Test func aCounterOrTimeThatWentBackwardsStartsANewBaseline() {
        var meter = StatsMeter()
        meter.record(reading(at: 5, busy: 10, frames: 100))
        meter.record(reading(at: 6, busy: 12, frames: 160))
        #expect(meter.cpuPercent == 50)
        // CPU time back: nothing new, and the next interval is measured from here.
        meter.record(reading(at: 7, busy: 3, frames: 220))
        #expect(meter.cpuPercent == 50 && meter.framesPerSecond == 60)
        meter.record(reading(at: 8, busy: 4, frames: 220))
        #expect(meter.cpuPercent == 25 && meter.framesPerSecond == 0)
        // Frames back.
        meter.record(reading(at: 9, busy: 5, frames: 10))
        #expect(meter.cpuPercent == 25)
        meter.record(reading(at: 10, busy: 5, frames: 40))
        #expect(meter.framesPerSecond == 30)
        // The clock back, or standing still.
        meter.record(reading(at: 9, busy: 6, frames: 50))
        meter.record(reading(at: 9, busy: 7, frames: 60))
        #expect(meter.framesPerSecond == 30)
        meter.record(reading(at: 10, busy: 8, frames: 70))
        #expect(meter.framesPerSecond == 10)
    }

    @Test func aChangeOfVCPUCountStartsANewBaseline() {
        var meter = StatsMeter()
        meter.record(reading(cpus: 1, at: 0, busy: 0))
        meter.record(reading(cpus: 2, at: 1, busy: 1))
        #expect(meter.cpuPercent == nil)
        meter.record(reading(cpus: 2, at: 2, busy: 2))
        #expect(meter.cpuPercent == 50)
        // A guest whose vCPUs have not come up has no share.
        var early = StatsMeter()
        early.record(reading(cpus: 0, at: 0))
        early.record(reading(cpus: 0, at: 1))
        #expect(early.cpuPercent == nil)
    }

    @Test func anotherBootDiscardsWhatTheLastGuestShowed() {
        var meter = StatsMeter()
        meter.record(reading(boot: 0, at: 0, busy: 0, frames: 0))
        meter.record(reading(boot: 0, at: 1, busy: 2, frames: 30))
        meter.record(memory: GuestMemorySample(totalBytes: 2000 << 20, availableBytes: 1000 << 20))
        #expect(meter.cpuPercent == 50 && meter.memoryInUseBytes == 1000 << 20)
        meter.record(reading(boot: 1, at: 2, busy: 0, frames: 30))
        #expect(meter.cpuPercent == nil && meter.framesPerSecond == nil)
        #expect(meter.memoryInUseBytes == nil && meter.history.isEmpty)
        #expect(meter.bootIndex == 1)
        // The interval across the two boots is no interval: the next one is.
        meter.record(reading(boot: 1, at: 3, busy: 1, frames: 60))
        #expect(meter.cpuPercent == 25 && meter.framesPerSecond == 30)
    }

    @Test func theMemoryInUseIsTheTotalLessWhatIsAvailable() {
        var meter = StatsMeter()
        meter.record(memory: GuestMemorySample(totalBytes: 1826 << 20, availableBytes: 1008 << 20))
        #expect(meter.memoryInUseBytes == 818 << 20)
        // An answer with no total is no answer.
        meter.record(memory: GuestMemorySample(totalBytes: 0, availableBytes: 0))
        #expect(meter.memoryInUseBytes == 818 << 20)
        // More available than total reads as nothing in use.
        meter.record(memory: GuestMemorySample(totalBytes: 100, availableBytes: 200))
        #expect(meter.memoryInUseBytes == 0)
    }

    @Test func theHeldRAMIsTheLastReadingsAndNothingBeforeTheKernelClaimsIt() {
        var meter = StatsMeter()
        meter.record(StatsSample(
            bootIndex: 0, processorCount: 4, timestampNanoseconds: 1, processorTimeNanoseconds: 0, presentedFrames: 0
        ))
        #expect(meter.ramHeldBytes == nil)
        meter.record(reading(at: 1))
        #expect(meter.ramHeldBytes == 1800 << 20)
    }

    /// The graphics figures need no second reading, and a reading without a
    /// budget (no GPU yet, or a runtime that gives none) has neither.
    @Test func theGraphicsMemoryIsTheLastReadingsWhileThereIsABudget() {
        var meter = StatsMeter()
        #expect(meter.gpuMemoryBudgetBytes == nil && meter.gpuMemoryUsedBytes == nil)
        var budgeted = reading(at: 1)
        budgeted.gpuMemoryBudgetBytes = 534 << 20
        budgeted.gpuMemoryUsedBytes = 412 << 20
        meter.record(budgeted)
        #expect(meter.gpuMemoryBudgetBytes == 534 << 20 && meter.gpuMemoryUsedBytes == 412 << 20)
        var none = reading(at: 2)
        none.gpuMemoryUsedBytes = 12 << 20
        meter.record(none)
        #expect(meter.gpuMemoryBudgetBytes == nil && meter.gpuMemoryUsedBytes == nil)
    }
}

struct StatsSamplingTests {
    @Test func theReadingsRunOnlyWhileEveryConditionHolds() {
        func wanted(
            _ viewers: Int = 1, active: Bool = true, capable: Bool = true, driver: Bool = true, running: Bool = true
        ) -> Bool {
            StatsSampling.isWanted(
                viewers: viewers, applicationActive: active, capable: capable, hasDriver: driver, running: running
            )
        }
        #expect(wanted())
        #expect(wanted(2))
        #expect(!wanted(0))
        #expect(!wanted(active: false))
        #expect(!wanted(capable: false))
        #expect(!wanted(driver: false))
        #expect(!wanted(running: false))
    }

    @Test func theFirstFigureShowsWithinAThirdOfASecond() {
        #expect(StatsSampling.firstGap < .milliseconds(300))
        #expect(StatsSampling.interval == .seconds(1))
        #expect(StatsSampling.interval * StatsSampling.memoryEvery == .seconds(2))
    }
}
