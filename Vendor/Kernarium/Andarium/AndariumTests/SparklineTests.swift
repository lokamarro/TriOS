// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Testing
@testable import Andarium

/// The CPU sparkline draws the same number of bars from the first reading
/// on: one per history slot, newest on the right, empty slots on the left.
struct SparklineTests {
    @Test func everySlotHasABarBeforeTheFirstReading() {
        let slots = Sparkline.slotValues([], slots: StatsMeter.historyLength)
        #expect(slots.count == StatsMeter.historyLength)
        #expect(slots.allSatisfy { $0 == nil })
    }

    @Test func aShortHistoryFillsFromTheRight() {
        let slots = Sparkline.slotValues([40, 55], slots: 10)
        #expect(slots.count == 10)
        #expect(slots.prefix(8).allSatisfy { $0 == nil })
        #expect(Array(slots.suffix(2)) == [40, 55])
    }

    @Test func theFreshPreviewSessionShowsDashesAndHoldsStill() {
        var readout = Readout(previewReadings: 0, memoryMiB: 2048)
        readout.tick(memoryMiB: 2048)
        #expect(readout.history.isEmpty)
        #expect(readout.cpuText == Readout.unknown)
        #expect(Sparkline.slotValues(readout.history, slots: StatsMeter.historyLength).allSatisfy { $0 == nil })
    }

    @Test func aShortPreviewSessionKeepsItsReadings() {
        var readout = Readout(previewReadings: 3, memoryMiB: 2048)
        readout.tick(memoryMiB: 2048)
        #expect(readout.history.count == 3)
        #expect(readout.cpu == readout.history.last)
    }

    @Test func aLongHistoryKeepsTheNewestValues() {
        let slots = Sparkline.slotValues(Array(1...12), slots: 10)
        #expect(slots == Array(3...12).map { Optional($0) })
    }
}
