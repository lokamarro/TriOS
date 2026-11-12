// SPDX-License-Identifier: GPL-2.0-or-later

import Testing

@testable import AndariumCore

struct StartReserveTests {
    @Test func theSpaceAStartNeedsIsWhatAPausedGuestNeedsToResume() {
        #expect(Limits.startReserveBytes == RunSpace.resumeAtBytes)
        // A first start needs more than any later one, so the ladder of
        // thresholds below it holds.
        #expect(RunSpace.resumeAtBytes <= Limits.firstStartReserveBytes)
        #expect(RunSpace.pauseBelowBytes < Limits.startReserveBytes)
    }

    @Test func theApplicationReadsTheSharedSizesAndFormatsThroughItsOwnModule() {
        #expect(Limits.bytesPerGiB == Bytes.gib)
        #expect(Formats.size(bytes: 3 * Bytes.gib / 2) == "1.5 GiB")
    }
}
