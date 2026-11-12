// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

struct ConsoleScannerTests {
    @Test func findsTheMilestonesInOrder() {
        var scanner = ConsoleScanner()
        let text = """
            [    0.000000] Linux version 6.18.54-g4bcc3653b86a (clang) #1 SMP
            [    1.2] init: init first stage started!
            [    9.3] init: starting service 'surfaceflinger'...
            [   22.9] init: processing action (sys.boot_completed=1) from (/system/etc/init/hw/init.rc:1)

            """
        #expect(scanner.feed(text) == [.kernelRunning, .androidStarting, .screenStarting, .ready])
    }

    @Test func recoveryIsReadyWhenItsServiceStarts() {
        var scanner = ConsoleScanner()
        let text = """
            [    0.000000] Linux version 6.18.54-g4bcc3653b86a (clang) #1 SMP
            [    0.6] init: init first stage started!
            [    0.8] init: starting service 'recovery'...

            """
        #expect(scanner.feed(text) == [.kernelRunning, .androidStarting, .ready])
    }

    @Test func onlyAndroidItselfCompletesABoot() {
        var scanner = ConsoleScanner()
        _ = scanner.feed("[    0.8] init: starting service 'recovery'...\n")
        #expect(scanner.bootCompletedCount == 0)
        _ = scanner.feed("[   22.9] init: processing action (sys.boot_completed=1) from (/x:1)\n")
        #expect(scanner.bootCompletedCount == 1)
        _ = scanner.feed("[   99.9] init: processing action (sys.boot_completed=1) from (/x:1)\n")
        #expect(scanner.bootCompletedCount == 2)
    }

    @Test func aLineSplitAcrossChunksIsStillFound() {
        var scanner = ConsoleScanner()
        #expect(scanner.feed("Linux ver").isEmpty)
        #expect(scanner.feed("sion 6.18.54\r\ninit: init first") == [.kernelRunning])
        #expect(scanner.feed(" stage started!\n") == [.androidStarting])
    }

    @Test func aMarkerInsideAnotherWordIsNotAMilestone() {
        var scanner = ConsoleScanner()
        #expect(scanner.feed("this is not Linux version output\n").isEmpty)
        #expect(scanner.feed("init: init first stage started\n").isEmpty)
    }

    @Test func anOverlongLineIsDroppedWithoutLosingTheNext() {
        var scanner = ConsoleScanner()
        let long = String(repeating: "x", count: ConsoleScanner.maximumLine * 3)
        #expect(scanner.feed(long + "\n").isEmpty)
        #expect(scanner.feed("Linux version 6.18\n") == [.kernelRunning])
    }

    @Test func dataIsDecodedAsUTF8() {
        var scanner = ConsoleScanner()
        #expect(scanner.feed(Data("Linux version 6.18\n".utf8)) == [.kernelRunning])
    }
}

struct BootTrackerTests {
    @Test func progressOnlyMovesForward() {
        var tracker = BootTracker()
        #expect(tracker.milestone == .starting)
        let advanced = tracker.reach(.androidStarting, atSeconds: 12)
        let backwards = tracker.reach(.kernelRunning, atSeconds: 13)
        let again = tracker.reach(.androidStarting, atSeconds: 14)
        #expect(advanced)
        #expect(!backwards)
        #expect(!again)
        #expect(tracker.milestone == .androidStarting)
        #expect(tracker.reached[.androidStarting] == 12)
        #expect(!tracker.isReady)
        let ready = tracker.reach(.ready, atSeconds: 40)
        #expect(ready)
        #expect(tracker.isReady)
    }
}

struct SystemInfoTests {
    private let manifest = """
        {
          "acceptance": "accepted",
          "android": {
            "build_id": "CP2A.260605.016", "build_number": 1793463711, "major": 17, "sdk": 37,
            "security_patch": "2026-10-01"
          },
          "inventory": [
            { "path": "guest/system.img", "role": "system-disk", "size": 1070096384 },
            { "path": "boot/kernel-command-line.txt", "role": "kernel-command-line", "size": 1332 }
          ],
          "kernel": { "page_size": 4096, "release": "6.18.54-g4bcc3653b86a" },
          "product_family": "lineage-24-mainline-6.18-4k",
          "release_id": "20261031-los24-r4"
        }
        """

    @Test func readsTheReleaseManifest() throws {
        let info = try SystemInfo(manifest: Data(manifest.utf8), appVersion: "1.0 (1)")
        #expect(info.version == "17")
        #expect(info.release == "20261031-los24-r4")
        #expect(info.buildLabel == "r4 (2026-10-31)")
        #expect(info.buildID == "CP2A.260605.016")
        #expect(info.kernel == "Linux 6.18.54")
        #expect(info.architecture == "arm64, 4 KiB pages")
        #expect(info.securityPatch == "1 October 2026")
        #expect(info.appVersion == "1.0 (1)")
        // A release published before the ABI list was recorded has none.
        #expect(info.abis.isEmpty)
    }

    @Test func theAbiListComesFromTheManifestInTheImagesOrder() throws {
        let listed = manifest.replacingOccurrences(
            of: "\"security_patch\": \"2026-10-01\"",
            with: "\"security_patch\": \"2026-10-01\", \"abi_list\": [\"arm64-v8a\", \"armeabi-v7a\", \"armeabi\"]"
        )
        let info = try SystemInfo(manifest: Data(listed.utf8), appVersion: "1")
        #expect(info.abis == ["arm64-v8a", "armeabi-v7a", "armeabi"])
        // Anything that is not a name is left out rather than shown.
        let odd = listed.replacingOccurrences(of: "\"armeabi\"]", with: "\"\", 7]")
        #expect(try SystemInfo(manifest: Data(odd.utf8), appVersion: "1").abis == ["arm64-v8a", "armeabi-v7a"])
    }

    @Test func theSecurityPatchIsWordedOrLeftOut() throws {
        let odd = manifest.replacingOccurrences(of: "2026-10-01", with: "October")
        #expect(try SystemInfo(manifest: Data(odd.utf8), appVersion: "1").securityPatch == nil)
        let december = manifest.replacingOccurrences(of: "2026-10-01", with: "2026-12-05")
        #expect(try SystemInfo(manifest: Data(december.utf8), appVersion: "1").securityPatch == "5 December 2026")
    }

    @Test func aMissingFieldNamesItself() {
        let broken = manifest.replacingOccurrences(of: "\"release_id\": \"20261031-los24-r4\"", with: "\"release_id\": \"\"")
        #expect(throws: SystemInfo.ManifestError.missing("release_id")) {
            try SystemInfo(manifest: Data(broken.utf8), appVersion: "1")
        }
        #expect(throws: SystemInfo.ManifestError.malformed) {
            try SystemInfo(manifest: Data("[1]".utf8), appVersion: "1")
        }
    }

    @Test func aFamilyThatIsNotLineageIsRefused() {
        let other = manifest.replacingOccurrences(of: "lineage-24-mainline-6.18-4k", with: "aosp-17-mainline")
        #expect(throws: SystemInfo.ManifestError.missing("product_family")) {
            try SystemInfo(manifest: Data(other.utf8), appVersion: "1")
        }
    }
}
