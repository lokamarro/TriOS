// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The runtime's record of how the previous session ended, read as the
/// application reads it at launch, the notice Home shows for it, and the
/// redaction its text goes through before it leaves the device.
struct SessionRecordTests {
    /// The record as the runtime writes it: a memory slot padded to 256
    /// bytes, then the session's state.
    private func record(_ state: String, memory: String = "memory at 1794114290 footprint 2147483648 peak 2415919104")
        -> String
    {
        var slot = "uml-session 2\n\(memory)\n"
        slot += String(repeating: " ", count: 255 - slot.utf8.count) + "\n"
        return slot + state
    }

    @Test func readsAPanicWithItsKernelOutput() throws {
        let text = record(
            """
            state ended
            end guest-panic
            status 6
            pid 46851
            started 1794114290
            ended 1794114295
            message sysrq triggered crash
            kernel-output
            [    0.566837] Kernel panic - not syncing: sysrq triggered crash
            Darwin UML panic: sysrq triggered crash

            """)
        let record = try #require(SessionRecord(text: text))
        #expect(!record.running && !record.orderly)
        #expect(record.end == "guest-panic" && record.status == 6 && record.processID == 46851)
        #expect(record.started == Date(timeIntervalSince1970: 1_794_114_290))
        #expect(record.ended == Date(timeIntervalSince1970: 1_794_114_295))
        #expect(record.message == "sysrq triggered crash")
        #expect(record.kernelOutput?.hasPrefix("[    0.566837] Kernel panic") == true)
        #expect(record.memory?.footprintBytes == 2 << 30 && record.memory?.availableBytes == nil)
        #expect(record.summary == "The previous session ended in a guest kernel panic: sysrq triggered crash.")
        #expect(record.notice(endedOutOfForeground: false) == SessionEndNotice(
            title: "Android's kernel panicked", text: "Android stopped with a kernel panic: sysrq triggered crash."))
    }

    @Test func aRecordStillRunningNearTheLimitWasEndedByIOS() throws {
        let near = try #require(SessionRecord(text: record(
            "state running\nend running\nstatus 0\npid 7\nstarted 1794114339\n",
            memory: "memory at 1794114400 footprint 5905580032 peak 5905580032 available 104857600")))
        #expect(near.running && !near.orderly && near.ended == nil && near.kernelOutput == nil)
        #expect(near.nearMemoryLimit)
        #expect(near.memory?.availableBytes == 100 << 20)
        #expect(near.summary.hasSuffix("it used 5632 MiB, 100 MiB left."))
        #expect(near.notice(endedOutOfForeground: false)?.title == "iOS ended Andarium")
        // Out of the foreground it is the pause marker's to report.
        #expect(near.notice(endedOutOfForeground: true) == nil)

        let roomy = try #require(SessionRecord(text: record(
            "state running\nend running\nstatus 0\npid 7\nstarted 1794114339\n",
            memory: "memory at 1794114400 footprint 2147483648 peak 2147483648 available 2147483648")))
        #expect(!roomy.nearMemoryLimit)
        #expect(roomy.notice(endedOutOfForeground: false)?.title == "Andarium stopped unexpectedly")
    }

    @Test func eachBadEndHasItsNotice() throws {
        let titles = [
            "kernel-abort": "Android's kernel crashed",
            "kernel-exit": "Android stopped",
            "hibernation-restore-failed": "Android could not resume",
            "host-fault": "Andarium crashed",
            "process-exit": "Andarium crashed",
        ]
        for (end, title) in titles {
            let ended = try #require(SessionRecord(text: record("state ended\nend \(end)\nstatus 11\npid 9\n")))
            #expect(ended.notice(endedOutOfForeground: false)?.title == title, "\(end)")
        }
    }

    @Test func anOrderlyEndHasNoNotice() throws {
        for end in ["powered-off", "restarted", "stopped"] {
            let ended = try #require(SessionRecord(text: record("state ended\nend \(end)\nstatus 0\npid 9\n")))
            #expect(ended.orderly && ended.message == nil)
            #expect(ended.notice(endedOutOfForeground: false) == nil)
        }
    }

    @Test func readsTheFirstFormatToo() throws {
        let first = try #require(SessionRecord(
            text: "uml-session 1\nstate ended\nend powered-off\nstatus 0\npid 9\nstarted 1\nended 2\n"))
        #expect(first.orderly && first.memory == nil)
    }

    @Test func readsTheTermsAndTheirHistory() throws {
        var slot = """
            uml-session 3
            memory at 1794210239 footprint 6441524416 peak 6442458280 available 926528
            terms guest 4090 gpu 1262/1251 age 0 graphics 905 media 0 cache 238 heaps 260 malloc 812/700 age 4 other 650 internal 5200 compressed 0
            peak-terms at 1794210239 footprint 6143 guest 4090 gpu 1262/1251 age 0 graphics 905 media 0 cache 238 heaps 260 malloc 812/700 age 4 other 650 internal 5200 compressed 0
            history seconds footprint available guest gpu graphics cache heaps malloc other
            h 484 6100 43 4090 1250 900 238 258 790 614
            h 486 6143 0 4090 1262 905 238 260 812 650

            """
        slot += String(repeating: " ", count: 4095 - slot.utf8.count) + "\n"
        let near = try #require(SessionRecord(
            text: slot + "state running\nend running\nstatus 0\npid 20373\nstarted 1794209753\n"))
        #expect(near.running && near.nearMemoryLimit)
        #expect(near.memory?.footprintBytes == 6_441_524_416 && near.memory?.availableBytes == 926_528)
        #expect(near.terms?.hasPrefix("guest 4090 gpu 1262/1251") == true)
        #expect(near.peakTerms?.hasPrefix("at 1794210239 footprint 6143 guest 4090") == true)
        #expect(near.notice(endedOutOfForeground: false)?.title == "iOS ended Andarium")
    }

    @Test func otherTextIsNoRecord() {
        #expect(SessionRecord(text: "") == nil)
        #expect(SessionRecord(text: "uml-session 4\nstate ended\nend powered-off\n") == nil)
        #expect(SessionRecord(text: "uml-session 2\nstatus 0\n") == nil)
        #expect(SessionRecord.read(at: URL(fileURLWithPath: "/nonexistent/session.txt")) == nil)
    }

    @Test func redactionTakesTheContainerPathsOut() {
        let container = "/private/var/mobile/Containers/Data/Application/0A1B2C3D-0000-4000-8000-0123456789AB"
        let text = """
            [    0.36] Not locking "\(container)/Library/Application Support/android/data.img" on the host
            opened /var/mobile/Containers/Data/Application/0A1B2C3D-0000-4000-8000-0123456789AB/tmp/x
            bundle /private/var/containers/Bundle/Application/11111111-2222-4333-8444-555555555555/Andarium.app
            mac /Users/NAME/Library/Containers/app/Data/x
            """
        let redacted = DiagnosticRedaction.redact(text, containers: [container])
        #expect(!redacted.contains("0A1B2C3D") && !redacted.contains("11111111") && !redacted.contains("/Users/NAME"))
        #expect(redacted.contains("Not locking \"<container>/Library/Application Support/android/data.img\""))
        #expect(redacted.contains("opened <container>/tmp/x"))
        #expect(redacted.contains("bundle <container>/Andarium.app"))
        #expect(redacted.contains("mac /Users/<user>/Library/Containers/app/Data/x"))
    }
}
