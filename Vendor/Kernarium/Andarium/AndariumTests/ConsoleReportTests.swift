// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing

@testable import Andarium

#if !APP_STORE

/// The lines the harness reads. An App Store build has none of them
/// (`Andarium/README.md#configurations`).
struct ConsoleReportTests {
    private let second: UInt64 = 1_000_000_000

    /// A reading of boot `boot` at `seconds`, with the vCPUs having computed
    /// `busy` seconds in all since the boot began and `frames` reaching the
    /// screen.
    private func reading(boot: UInt32 = 0, at seconds: UInt64, busy: Double = 0, frames: UInt64 = 0) -> StatsSample {
        StatsSample(
            bootIndex: boot, processorCount: 4, timestampNanoseconds: seconds * second,
            processorTimeNanoseconds: UInt64(busy * Double(second)), presentedFrames: frames,
            ramBytes: 2048 << 20, ramHeldBytes: 1800 << 20
        )
    }

    @Test func thermalLine() {
        #expect(
            ConsoleReport.thermal(phase: .start, state: .nominal, uptime: 12.3456)
                == "Andarium thermal state: phase=start state=nominal uptime=12.346"
        )
        #expect(
            ConsoleReport.thermal(phase: .finish, state: .serious, uptime: 0)
                == "Andarium thermal state: phase=finish state=serious uptime=0.000"
        )
    }

    @Test func runtimeLine() {
        #expect(ConsoleReport.runtimeReturned(0) == "Andarium: runtime returned 0")
        #expect(ConsoleReport.runtimeReturned(-3) == "Andarium: runtime returned -3")
    }

    @Test func focusLines() {
        #expect(ConsoleReport.application(active: true) == "Andarium: application active")
        #expect(ConsoleReport.application(active: false) == "Andarium: application inactive")
    }

    @Test func theStatisticsLineStatesWhatTheScreensShowInMebibytes() {
        var meter = StatsMeter()
        meter.record(reading(boot: 2, at: 0))
        meter.record(reading(boot: 2, at: 1, busy: 1.36, frames: 59))
        meter.record(memory: GuestMemorySample(totalBytes: 1826 << 20, availableBytes: 1008 << 20))
        #expect(
            ConsoleReport.statistics(meter, guestMemoryMiB: 2048)
                == "Andarium stats: cpu=34% fps=59 memory-used=818 memory=2048 held=1800 boot=2"
        )
    }

    @Test func aStatisticsFigureNotYetKnownIsADash() {
        #expect(
            ConsoleReport.statistics(StatsMeter(), guestMemoryMiB: 1536)
                == "Andarium stats: cpu=- fps=- memory-used=- memory=1536 held=- boot=-"
        )
        var meter = StatsMeter()
        meter.record(reading(at: 0))
        #expect(
            ConsoleReport.statistics(meter, guestMemoryMiB: 1536)
                == "Andarium stats: cpu=- fps=- memory-used=- memory=1536 held=1800 boot=0"
        )
    }
}

#endif
