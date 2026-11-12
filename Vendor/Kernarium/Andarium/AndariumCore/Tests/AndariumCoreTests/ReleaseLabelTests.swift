// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

struct ReleaseLabelTests {
    @Test func theAcceptedReleaseReadsAsItsRevisionAndDate() throws {
        let label = try #require(ReleaseLabel(releaseID: "20261109-los24-r20"))
        #expect(label.revision == 20)
        #expect(label.revisionText == "r20")
        #expect(label.dateText == "2026-11-09")
        #expect(ReleaseLabel.revision(of: "20261109-los24-r20") == "r20")
        #expect(ReleaseLabel.build(of: "20261109-los24-r20") == "r20 (2026-11-09)")
    }

    @Test func anUpdateNamesBothRevisions() {
        // r19 was never accepted: the update from r18 goes to r20.
        let from = "20261108-los24-r18"
        let to = "20261109-los24-r20"
        #expect("Updating from \(ReleaseLabel.revision(of: from)) to \(ReleaseLabel.revision(of: to))" == "Updating from r18 to r20")
    }

    @Test func aTwoDigitRevisionIsNotCutShort() {
        #expect(ReleaseLabel.revision(of: "20270105-los24-r103") == "r103")
        #expect(ReleaseLabel.revision(of: "20261031-los24-r4") == "r4")
        #expect(ReleaseLabel.build(of: "20270105-los24-r103") == "r103 (2027-01-05)")
    }

    @Test(arguments: [
        "unavailable", "unknown", "test", "r2", "", "20261024",
        // A future scheme, or a part that is not what the scheme says.
        "20261109-los24-r20-1", "20261109-r20", "2026-11-09-los24-r20", "20261109--r20",
        "20261109-los24-20", "20261109-los24-r", "20261109-los24-R20", "20261109-los24-r020",
        "20261109-los24-r0", "20261109-los24-r2x", "20261109-LOS24-r20", "20261109-los_24-r20",
        "2026110-los24-r20", "202611091-los24-r20", "20261309-los24-r20", "20261100-los24-r20",
        "19991109-los24-r20", "٢٠٢٦١١٠٩-los24-r20",
    ])
    func anIDOutsideTheSchemeIsShownAsItIs(_ id: String) {
        #expect(ReleaseLabel(releaseID: id) == nil)
        #expect(ReleaseLabel.revision(of: id) == id)
        #expect(ReleaseLabel.build(of: id) == id)
    }

    @Test func theSystemNamesItsOwnRelease() throws {
        let system = SystemInfo(
            version: "17", release: "20261109-los24-r20",
            kernel: "Linux 6.18", architecture: "arm64", appVersion: "1"
        )
        #expect(system.releaseRevision == "r20")
        #expect(system.buildLabel == "r20 (2026-11-09)")
        // The ID itself is untouched.
        #expect(system.release == "20261109-los24-r20")

        let unavailable = SystemInfo(
            version: "", release: "unavailable", kernel: "", architecture: "", appVersion: "1"
        )
        #expect(unavailable.releaseRevision == "unavailable")
        #expect(unavailable.buildLabel == "unavailable")
    }
}
