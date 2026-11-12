// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// Holds the loop at its wait between readings until a test lets it go, and
/// records how long each wait was.
@MainActor
private final class SleepGate {
    private(set) var waits: [Duration] = []
    private var held: CheckedContinuation<Void, Never>?

    func sleep(_ duration: Duration) async {
        waits.append(duration)
        await withTaskCancellationHandler {
            await withCheckedContinuation { held = $0 }
        } onCancel: {
            Task { @MainActor in self.release() }
        }
    }

    var isWaiting: Bool { held != nil }

    func release() {
        let continuation = held
        held = nil
        continuation?.resume()
    }
}

/// The statistics loop against a scripted runtime, with the pause between
/// readings held by the test: no reading without a viewer, the quarter-second
/// then one-second cadence with the memory after every other reading, the loop
/// stopping with the foreground, with a guest that is not running and with its
/// last viewer, a memory the kernel does not give keeping the last figure, and
/// a new boot starting the figures over. `StatsMeter` and `StatsSampling` are
/// tested in `andarium-core`, and `ConsoleReport`, the harness's console
/// lines, in the application (`ConsoleReportTests`).
@MainActor
struct StatisticsLoopTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    /// A reading `milliseconds` into a boot of a guest with two vCPUs, one of
    /// which has been busy for `busy` of them, and `frames` frames shown.
    private func reading(
        _ milliseconds: UInt64, busy: UInt64, frames: UInt64, boot: UInt32 = 0, cpus: UInt32 = 2
    ) -> StatsSample {
        StatsSample(
            bootIndex: boot, processorCount: cpus, timestampNanoseconds: milliseconds * 1_000_000,
            processorTimeNanoseconds: busy * 1_000_000, presentedFrames: frames
        )
    }

    private func memory(usedMiB: UInt64) -> GuestMemorySample {
        GuestMemorySample(
            totalBytes: 2048 * Limits.bytesPerMiB, availableBytes: (2048 - usedMiB) * Limits.bytesPerMiB
        )
    }

    /// A model that runs a guest, read through `driver` and paced by `gate`.
    private func runningModel(
        _ driver: StubDriver?, gate: SleepGate, capabilities: Capabilities = .device
    ) -> AppModel {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: capabilities,
            createRequirementBytes: 1
        )
        model.driver = driver
        model.statisticsSleep = { duration in await gate.sleep(duration) }
        model.phase = .running(.normal)
        return model
    }

    /// Lets the loop run until `condition` holds, for at most five seconds.
    private func until(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(5)
        while !condition() {
            if ContinuousClock.now > deadline { return false }
            try? await Task.sleep(for: .milliseconds(1))
        }
        return true
    }

    /// Time enough for a loop that should not run to show that it does.
    private func settle() async {
        try? await Task.sleep(for: .milliseconds(60))
    }

    @Test func nothingIsReadWhileNoViewShowsTheFigures() async {
        let gate = SleepGate()
        let driver = StubDriver(plan: .normal)
        let model = runningModel(driver, gate: gate)
        await settle()
        #expect(driver.statisticsAsked == 0)
        #expect(driver.memoryAsked == 0)
        #expect(gate.waits.isEmpty)
        model.statistics(.pill, visible: true)
        #expect(await until { gate.isWaiting })
        model.statistics(.pill, visible: false)
        await settle()
        let asked = driver.statisticsAsked
        gate.release()
        await settle()
        #expect(driver.statisticsAsked == asked)
    }

    @Test func theReadingsStartWithTwoAQuarterSecondApartAndThenGoOnceASecond() async {
        let gate = SleepGate()
        let driver = StubDriver(plan: .normal)
        driver.samples = [.some(reading(0, busy: 0, frames: 0))]
        let model = runningModel(driver, gate: gate)
        model.statistics(.pill, visible: true)
        #expect(await until { gate.waits.count == 1 && gate.isWaiting })
        #expect(driver.statisticsAsked == 1)
        #expect(driver.memoryAsked == 0, "the guest is asked for its memory after the second reading")
        gate.release()
        #expect(await until { gate.waits.count == 2 && gate.isWaiting })
        #expect(driver.statisticsAsked == 2)
        #expect(driver.memoryAsked == 1)
        gate.release()
        #expect(await until { gate.waits.count == 3 && gate.isWaiting })
        #expect(driver.statisticsAsked == 3)
        #expect(driver.memoryAsked == 1, "the memory comes with every second reading")
        gate.release()
        #expect(await until { gate.waits.count == 4 && gate.isWaiting })
        #expect(driver.statisticsAsked == 4)
        #expect(driver.memoryAsked == 2)
        #expect(gate.waits == [.milliseconds(250), .seconds(1), .seconds(1), .seconds(1)])
        model.statistics(.pill, visible: false)
    }

    @Test func theFiguresAreWhatTheReadingsSay() async {
        let gate = SleepGate()
        let driver = StubDriver(plan: .normal)
        // 250 ms later: 125 ms of work on two vCPUs is a quarter of them,
        // and 15 frames are 60 a second. The GPU has its budget by then.
        var budgeted = reading(250, busy: 125, frames: 15)
        budgeted.gpuMemoryBudgetBytes = 534 * Limits.bytesPerMiB
        budgeted.gpuMemoryUsedBytes = 412 * Limits.bytesPerMiB
        driver.samples = [.some(reading(0, busy: 0, frames: 0)), .some(budgeted)]
        driver.memories = [.some(memory(usedMiB: 818))]
        let model = runningModel(driver, gate: gate)
        #expect(model.session.readout.cpu == nil, "no figure before the first reading")
        #expect(model.session.readout.cpuText == "\u{2013}")
        model.statistics(.settings, visible: true)
        #expect(await until { gate.waits.count == 1 && gate.isWaiting })
        #expect(model.session.readout.cpu == nil, "one reading is no rate")
        #expect(model.session.readout.vramText == "\u{2013}", "no budget yet is no figure")
        #expect(model.session.readout.vramFraction == nil)
        gate.release()
        #expect(await until { gate.waits.count == 2 && gate.isWaiting })
        let readout = model.session.readout
        #expect(readout.cpu == 25)
        #expect(readout.fps == 60)
        #expect(readout.history == [25])
        #expect(readout.memoryUsedBytes == 818 * Limits.bytesPerMiB)
        #expect(readout.memoryText(of: 2048) == "818 MiB of 2 GiB")
        #expect(readout.vramText == "412 MiB of 534 MiB")
        #expect(readout.vramUsedText == "412 MiB")
        #expect(readout.vramFraction.map { abs($0 - 412.0 / 534) < 1e-9 } == true)
        model.statistics(.settings, visible: false)
    }

    @Test func twoViewsShareOneLoop() async {
        let gate = SleepGate()
        let driver = StubDriver(plan: .normal)
        driver.samples = [.some(reading(0, busy: 0, frames: 0))]
        let model = runningModel(driver, gate: gate)
        model.statistics(.pill, visible: true)
        model.statistics(.settings, visible: true)
        #expect(await until { gate.waits.count == 1 && gate.isWaiting })
        #expect(driver.statisticsAsked == 1)
        // One goes, one stays: the reading goes on.
        model.statistics(.settings, visible: false)
        gate.release()
        #expect(await until { gate.waits.count == 2 && gate.isWaiting })
        #expect(driver.statisticsAsked == 2)
        // The last one goes: the loop ends, and nothing more is read.
        model.statistics(.pill, visible: false)
        gate.release()
        await settle()
        #expect(driver.statisticsAsked == 2)
        #expect(gate.waits.count == 2)
    }

    @Test func aMemoryTheKernelDoesNotGiveKeepsTheLastFigure() async {
        let gate = SleepGate()
        let driver = StubDriver(plan: .normal)
        driver.samples = [.some(reading(0, busy: 0, frames: 0))]
        driver.memories = [nil, .some(memory(usedMiB: 700)), nil]
        let model = runningModel(driver, gate: gate)
        model.statistics(.pill, visible: true)
        for wait in 1...2 {
            #expect(await until { gate.waits.count == wait && gate.isWaiting })
            gate.release()
        }
        #expect(await until { gate.waits.count == 3 && gate.isWaiting })
        #expect(driver.memoryAsked == 1)
        #expect(model.session.readout.memoryUsedBytes == nil, "a memory never given stays unknown")
        for wait in 3...4 {
            #expect(await until { gate.waits.count == wait && gate.isWaiting })
            gate.release()
        }
        #expect(await until { gate.waits.count == 5 && gate.isWaiting })
        #expect(driver.memoryAsked == 2)
        #expect(model.session.readout.memoryUsedBytes == 700 * Limits.bytesPerMiB)
        for wait in 5...6 {
            #expect(await until { gate.waits.count == wait && gate.isWaiting })
            gate.release()
        }
        #expect(await until { gate.waits.count == 7 && gate.isWaiting })
        #expect(driver.memoryAsked == 3)
        #expect(
            model.session.readout.memoryUsedBytes == 700 * Limits.bytesPerMiB,
            "a memory that cannot be had keeps the last figure"
        )
        model.statistics(.pill, visible: false)
    }

    @Test func leavingTheForegroundStopsTheReadingsAndReturningStartsOver() async {
        let gate = SleepGate()
        let driver = StubDriver(plan: .normal)
        driver.samples = [.some(reading(0, busy: 0, frames: 0)), .some(reading(250, busy: 125, frames: 15))]
        let model = runningModel(driver, gate: gate)
        model.statistics(.pill, visible: true)
        #expect(await until { gate.waits.count == 1 && gate.isWaiting })
        gate.release()
        #expect(await until { gate.waits.count == 2 && gate.isWaiting })
        #expect(model.session.readout.cpu == 25)
        model.applicationActive = false
        await settle()
        #expect(driver.statisticsAsked == 2)
        gate.release()
        await settle()
        #expect(driver.statisticsAsked == 2, "nothing is read while the application is not active")
        model.applicationActive = true
        // A new run of readings: two a quarter second apart again, and the
        // rate waits for the second since none spans the gap.
        #expect(await until { gate.waits.count == 3 && gate.isWaiting })
        #expect(gate.waits.last == .milliseconds(250))
        #expect(driver.statisticsAsked == 3)
        model.statistics(.pill, visible: false)
    }

    @Test func aGuestThatIsNotRunningIsNotRead() async {
        let gate = SleepGate()
        let driver = StubDriver(plan: .normal)
        driver.samples = [.some(reading(0, busy: 0, frames: 0))]
        let model = runningModel(driver, gate: gate)
        model.phase = .booting(.normal)
        model.statistics(.pill, visible: true)
        await settle()
        #expect(driver.statisticsAsked == 0, "a guest that is still booting")
        model.phase = .running(.normal)
        #expect(await until { gate.waits.count == 1 && gate.isWaiting })
        #expect(driver.statisticsAsked == 1)
        // It ends (a clean shutdown returns to Home): the loop ends with it,
        // without waiting for its next turn.
        model.phase = .home
        await settle()
        gate.release()
        await settle()
        #expect(driver.statisticsAsked == 1)
        // A restart in place reads the new guest from the start.
        model.phase = .running(.normal)
        #expect(await until { gate.waits.count == 2 && gate.isWaiting })
        #expect(driver.statisticsAsked == 2)
        model.statistics(.pill, visible: false)
    }

    @Test func aRuntimeWithoutStatisticsOrADriverIsNotRead() async {
        let gate = SleepGate()
        let driver = StubDriver(plan: .normal)
        driver.samples = [.some(reading(0, busy: 0, frames: 0))]
        var without = Capabilities.device
        without.statistics = false
        let incapable = runningModel(driver, gate: gate, capabilities: without)
        incapable.statistics(.pill, visible: true)
        let driverless = runningModel(nil, gate: gate)
        driverless.statistics(.pill, visible: true)
        await settle()
        #expect(driver.statisticsAsked == 0)
        #expect(gate.waits.isEmpty)
        incapable.statistics(.pill, visible: false)
        driverless.statistics(.pill, visible: false)
    }

    @Test func aNewBootStartsTheFiguresOver() async {
        let gate = SleepGate()
        let driver = StubDriver(plan: .normal)
        driver.samples = [
            .some(reading(0, busy: 0, frames: 0)), .some(reading(250, busy: 125, frames: 15)),
            .some(reading(1250, busy: 625, frames: 75, boot: 1)),
        ]
        driver.memories = [.some(memory(usedMiB: 818))]
        let model = runningModel(driver, gate: gate)
        model.statistics(.pill, visible: true)
        for wait in 1...2 {
            #expect(await until { gate.waits.count == wait && gate.isWaiting })
            gate.release()
        }
        #expect(await until { gate.waits.count == 3 && gate.isWaiting })
        let readout = model.session.readout
        #expect(readout.cpu == nil, "the first reading of a new boot has no rate")
        #expect(readout.history.isEmpty)
        #expect(readout.memoryUsedBytes == nil, "the last guest's memory is not this one's")
        model.statistics(.pill, visible: false)
    }
}
