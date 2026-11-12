// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// What Andarium knows about its sessions, as the application uses it: a bug
/// report carries the session records and MetricKit's reports inside
/// Android's own zip, without the application's container paths, and the
/// launch after a bad end says so on Home, once. A stub stands for the
/// runtime; the records are written as the runtime writes them.
@Suite(.serialized)
@MainActor
struct SessionDiagnosticsTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func model(guest: Bool = true) -> (AppModel, StubDriver) {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: guest ? Guest.fresh(on: host) : nil, host: host, system: system, capabilities: .device,
            createRequirementBytes: 1
        )
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        return (model, driver)
    }

    /// A diagnostics folder inside the application's container, as the real
    /// one is.
    private func diagnostics() throws -> URL {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-diagnostics-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }

    /// A record as the runtime writes it: the memory slot, padded to 256
    /// bytes, then the state.
    private func record(_ state: String, memory: String) -> String {
        var slot = "uml-session 2\n\(memory)\n"
        slot += String(repeating: " ", count: 255 - slot.utf8.count) + "\n"
        return slot + state
    }

    private func panicRecord(mentioning path: String) -> String {
        record(
            """
            state ended
            end guest-panic
            status 6
            pid 4242
            started 1794114290
            ended 1794114295
            message Attempted to kill init! exitcode=0x0000000b
            kernel-output
            [    0.36] Not locking "\(path)/data.img" on the host
            [   12.50] Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b

            """, memory: "memory at 1794114294 footprint 3221225472 peak 3489660928 available 1073741824")
    }

    private func write(_ text: String, to url: URL) throws {
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try text.write(to: url, atomically: true, encoding: .utf8)
    }

    /// Waits, up to a bound, for `condition` on the main actor.
    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return condition()
    }

    // MARK: Bug report

    @Test func aBugReportCarriesTheRecordsAndReportsWithoutTheContainer() async throws {
        let (model, driver) = model()
        let directory = try diagnostics()
        defer { try? FileManager.default.removeItem(at: directory) }
        let containers = AppEntry.containerPaths()
        // The launch after a panic, then a boot that is still running.
        try write(panicRecord(mentioning: directory.path), to: directory.appendingPathComponent("session.txt"))
        model.adoptDiagnostics(in: directory, containers: containers, endedOutOfForeground: false)
        try write(
            record(
                "state running\nend running\nstatus 0\npid 4343\nstarted 1794114400\n",
                memory: "memory at 1794114410 footprint 2147483648 peak 2147483648 available 3221225472"),
            to: directory.appendingPathComponent("session.txt"))
        let metrics = #"{"applicationExitMetrics":{"foregroundExitData":{"cumulativeMemoryResourceLimitExitCount":1}},"# +
            #""file":"\#(NSHomeDirectory())/Library/x"}"#
        MetricKitReports.keep(Data(metrics.utf8), named: "20261108T101010Z-metrics.json", in: directory)

        model.phase = .running(.normal)
        let android = String(repeating: "== dumpstate: 2026-11-08 10:10:10\n", count: 500)
        let file = try TestZip.staged(entries: ["bugreport-test.txt": Data(android.utf8)])
        driver.bugReport = { _ in file }
        var offered: URL?
        var finishedSharing: (@MainActor () -> Void)?
        model.presentShare = { url, done in
            offered = url
            finishedSharing = done
            return true
        }
        model.createBugReport()
        #expect(await eventually { offered != nil })
        #expect(offered == file && model.alert == nil)

        let entries = try TestZip.entries(of: file)
        #expect(entries.map(\.name) == [
            "bugreport-test.txt", "andarium/session-previous.txt", "andarium/session-current.txt",
            "andarium/metrickit/20261108T101010Z-metrics.json",
        ])
        // Android's own entry is untouched.
        #expect(entries.first?.data == Data(android.utf8))
        let text = entries.dropFirst().map { String(decoding: $0.data, as: UTF8.self) }
        #expect(text[0].contains("end guest-panic") && text[0].contains("Not locking \"<container>/"))
        #expect(text[1].contains("state running") && text[1].contains("available 3221225472"))
        #expect(text[2].contains("cumulativeMemoryResourceLimitExitCount") && text[2].contains("<container>/Library/x"))
        for entry in text {
            #expect(!entry.contains(NSHomeDirectory()) && !entry.contains(directory.path))
        }
        finishedSharing?()
        #expect(!FileManager.default.fileExists(atPath: file.path))
    }

    @Test func aReportTheArchiveCannotTakeIsSharedAsAndroidMadeIt() async throws {
        let (model, driver) = model()
        let directory = try diagnostics()
        defer { try? FileManager.default.removeItem(at: directory) }
        try write("uml-session 1\nstate running\nend running\n", to: directory.appendingPathComponent("session.txt"))
        model.diagnosticsDirectory = directory
        model.phase = .running(.normal)
        try FileManager.default.createDirectory(at: BugReportStaging.directory, withIntermediateDirectories: true)
        let file = BugReportStaging.directory.appendingPathComponent("test-\(UUID().uuidString).zip")
        let truncated = Data([0x50, 0x4B, 5, 6])
        try truncated.write(to: file)
        driver.bugReport = { _ in file }
        var offered: URL?
        var finishedSharing: (@MainActor () -> Void)?
        model.presentShare = { url, done in
            offered = url
            finishedSharing = done
            return true
        }
        model.createBugReport()
        #expect(await eventually { offered != nil })
        #expect(offered == file && model.alert == nil)
        #expect(try Data(contentsOf: file) == truncated)
        let staged = try FileManager.default.contentsOfDirectory(atPath: BugReportStaging.directory.path)
        #expect(!staged.contains { $0.hasSuffix(".amending") })
        finishedSharing?()
    }

    // MARK: Notice

    @Test func aBadEndShowsItsNoticeOnceAtTheNextLaunch() throws {
        let directory = try diagnostics()
        defer { try? FileManager.default.removeItem(at: directory) }
        let current = directory.appendingPathComponent("session.txt")
        try write(panicRecord(mentioning: directory.path), to: current)

        let (launched, _) = model()
        launched.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
        #expect(launched.notice == .sessionEnded(SessionEndNotice(
            title: "Android's kernel panicked",
            text: "Android stopped with a kernel panic: Attempted to kill init! exitcode=0x0000000b.")))
        #expect(launched.diagnosticsDirectory == directory)
        // The record is kept for the bug report, and the next boot writes its own.
        #expect(!FileManager.default.fileExists(atPath: current.path))
        #expect(FileManager.default.fileExists(atPath: directory.appendingPathComponent("previous-session.txt").path))

        // A second launch with no boot between says nothing.
        let (again, _) = model()
        again.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
        #expect(again.notice == nil)
    }

    @Test func aSessionStillRunningSaysWhoEndedIt() throws {
        let directory = try diagnostics()
        defer { try? FileManager.default.removeItem(at: directory) }
        let current = directory.appendingPathComponent("session.txt")
        let nearLimit = record(
            "state running\nend running\nstatus 0\npid 7\nstarted 1794114339\n",
            memory: "memory at 1794114400 footprint 5905580032 peak 5905580032 available 104857600")

        try write(nearLimit, to: current)
        let (foreground, _) = model()
        foreground.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
        #expect(foreground.notice == .sessionEnded(SessionEndNotice(
            title: "iOS ended Andarium",
            text: "Android was using almost all the memory iOS allows Andarium. A smaller memory size is safer.")))

        // Ended paused out of the foreground: the pause marker's notice stays.
        try write(nearLimit, to: current)
        let (paused, _) = model()
        paused.notice = .closedInBackground
        paused.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: true)
        #expect(paused.notice == .closedInBackground)

        // An orderly end, or no guest left to start, shows nothing.
        try write(record("state ended\nend powered-off\nstatus 0\npid 8\n", memory: "memory at 1"), to: current)
        let (orderly, _) = model()
        orderly.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
        #expect(orderly.notice == nil)
        try write(nearLimit, to: current)
        let (erased, _) = model(guest: false)
        erased.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
        #expect(erased.notice == nil)
    }
}

/// Zips as Android's bug report makes them, deflated, and their entries read
/// back, for the tests: iOS has no zip tool.
enum TestZip {
    struct Entry {
        var name: String
        var data: Data
    }

    enum Failure: Error {
        case malformed(String)
    }

    /// A deflated zip of `entries` in the bug report staging folder.
    static func staged(entries: KeyValuePairs<String, Data>) throws -> URL {
        var body = Data()
        var central = Data()
        for (name, data) in entries {
            let compressed = try (data as NSData).compressed(using: .zlib) as Data
            let nameBytes = Data(name.utf8)
            let offset = UInt32(body.count)
            var header = Data()
            header.append(le: UInt32(0x0403_4b50))
            header.append(le: UInt16(20))
            header.append(le: UInt16(0))
            header.append(le: UInt16(8))
            header.append(le: UInt32(0))  // time and date
            header.append(le: crc32(data))
            header.append(le: UInt32(compressed.count))
            header.append(le: UInt32(data.count))
            header.append(le: UInt16(nameBytes.count))
            header.append(le: UInt16(0))
            body.append(header)
            body.append(nameBytes)
            body.append(compressed)
            central.append(le: UInt32(0x0201_4b50))
            central.append(le: UInt16(20))
            central.append(header.subdata(in: 4..<header.count))
            central.append(le: UInt16(0))  // comment
            central.append(le: UInt16(0))  // disk
            central.append(le: UInt16(0))
            central.append(le: UInt32(0))
            central.append(le: offset)
            central.append(nameBytes)
        }
        var end = Data()
        end.append(le: UInt32(0x0605_4b50))
        end.append(le: UInt32(0))
        end.append(le: UInt16(entries.count))
        end.append(le: UInt16(entries.count))
        end.append(le: UInt32(central.count))
        end.append(le: UInt32(body.count))
        end.append(le: UInt16(0))
        try FileManager.default.createDirectory(at: BugReportStaging.directory, withIntermediateDirectories: true)
        let url = BugReportStaging.directory.appendingPathComponent("test-\(UUID().uuidString).zip")
        try (body + central + end).write(to: url)
        return url
    }

    /// The entries in central directory order, each checked against its CRC.
    static func entries(of url: URL) throws -> [Entry] {
        let zip = try Data(contentsOf: url)
        guard zip.count >= 22, zip.uint32(at: zip.count - 22) == 0x0605_4b50 else {
            throw Failure.malformed("no end record")
        }
        let count = Int(zip.uint16(at: zip.count - 12))
        var index = Int(zip.uint32(at: zip.count - 6))
        var entries: [Entry] = []
        for _ in 0..<count {
            guard zip.uint32(at: index) == 0x0201_4b50 else { throw Failure.malformed("central entry") }
            let method = zip.uint16(at: index + 10), crc = zip.uint32(at: index + 16)
            let size = Int(zip.uint32(at: index + 20))
            let nameLength = Int(zip.uint16(at: index + 28)), extra = Int(zip.uint16(at: index + 30))
            let comment = Int(zip.uint16(at: index + 32)), local = Int(zip.uint32(at: index + 42))
            let name = String(decoding: zip.subdata(in: (index + 46)..<(index + 46 + nameLength)), as: UTF8.self)
            guard zip.uint32(at: local) == 0x0403_4b50 else { throw Failure.malformed("local entry \(name)") }
            let start = local + 30 + Int(zip.uint16(at: local + 26)) + Int(zip.uint16(at: local + 28))
            let stored = zip.subdata(in: start..<(start + size))
            let data = method == 8 ? try (stored as NSData).decompressed(using: .zlib) as Data : stored
            guard crc32(data) == crc else { throw Failure.malformed("checksum of \(name)") }
            entries.append(Entry(name: name, data: data))
            index += 46 + nameLength + extra + comment
        }
        return entries
    }

    static func crc32(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xFFFF_FFFF
        for byte in data {
            crc ^= UInt32(byte)
            for _ in 0..<8 { crc = crc & 1 != 0 ? 0xEDB8_8320 ^ (crc >> 1) : crc >> 1 }
        }
        return crc ^ 0xFFFF_FFFF
    }
}

extension Data {
    fileprivate func uint16(at index: Int) -> UInt16 {
        UInt16(self[startIndex + index]) | UInt16(self[startIndex + index + 1]) << 8
    }

    fileprivate func uint32(at index: Int) -> UInt32 {
        UInt32(uint16(at: index)) | UInt32(uint16(at: index + 2)) << 16
    }

    fileprivate mutating func append<Value: FixedWidthInteger>(le value: Value) {
        Swift.withUnsafeBytes(of: value.littleEndian) { append(contentsOf: $0) }
    }
}
