// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Adds files to the zip Android's bug report arrives as, so that what the
/// host knows travels in the same file.
///
/// The entries go after the archive's last one, stored, and a new central
/// directory follows them: the archive's own entries are not read, moved or
/// rewritten. The work happens on a clone of the file, which replaces the
/// original only once it is complete, so a failure leaves the report as
/// Android made it. Only a plain archive is amended: one disk, no ZIP64
/// records, the central directory right before its end record.
public enum BugReportArchive {
    public struct Entry: Equatable, Sendable {
        public var name: String
        public var data: Data

        public init(name: String, data: Data) {
            self.name = name
            self.data = data
        }
    }

    public enum Failure: Error, Equatable {
        case notAnArchive
        case unsupported(String)
    }

    public static func append(_ entries: [Entry], to url: URL, date: Date = Date()) throws {
        guard !entries.isEmpty else { return }
        let copy = url.deletingLastPathComponent()
            .appendingPathComponent(".\(url.lastPathComponent).amending")
        try? FileManager.default.removeItem(at: copy)
        try FileManager.default.copyItem(at: url, to: copy)
        do {
            try amend(copy, with: entries, date: date)
            _ = try FileManager.default.replaceItemAt(url, withItemAt: copy)
        } catch {
            try? FileManager.default.removeItem(at: copy)
            throw error
        }
    }

    private static let endSignature: UInt32 = 0x0605_4b50
    private static let zip64LocatorSignature: UInt32 = 0x0706_4b50
    private static let centralSignature: UInt32 = 0x0201_4b50
    private static let localSignature: UInt32 = 0x0403_4b50

    private static func amend(_ url: URL, with entries: [Entry], date: Date) throws {
        let handle = try FileHandle(forUpdating: url)
        defer { try? handle.close() }
        let size = try handle.seekToEnd()
        guard size >= 22 else { throw Failure.notAnArchive }
        let tailSize = min(size, 22 + 0xFFFF)
        try handle.seek(toOffset: size - tailSize)
        let tail = try handle.read(upToCount: Int(tailSize)) ?? Data()
        // The end record is the last signature whose comment reaches the end.
        var found: Int?
        var index = tail.count - 22
        while index >= 0 {
            if tail.uint32(at: index) == endSignature,
                index + 22 + Int(tail.uint16(at: index + 20)) == tail.count
            {
                found = index
                break
            }
            index -= 1
        }
        guard let end = found else { throw Failure.notAnArchive }
        let endOffset = size - tailSize + UInt64(end)
        let disk = tail.uint16(at: end + 4), directoryDisk = tail.uint16(at: end + 6)
        let diskEntries = tail.uint16(at: end + 8), count = tail.uint16(at: end + 10)
        let directorySize = tail.uint32(at: end + 12), directoryOffset = tail.uint32(at: end + 16)
        let comment = tail.subdata(in: (end + 22)..<tail.count)
        if disk != 0 || directoryDisk != 0 || diskEntries != count {
            throw Failure.unsupported("a multi-disk archive")
        }
        if count == 0xFFFF || directorySize == 0xFFFF_FFFF || directoryOffset == 0xFFFF_FFFF
            || (end >= 20 && tail.uint32(at: end - 20) == zip64LocatorSignature)
        {
            throw Failure.unsupported("a ZIP64 archive")
        }
        guard UInt64(directoryOffset) + UInt64(directorySize) == endOffset else {
            throw Failure.unsupported("data between the central directory and its end")
        }
        guard Int(count) + entries.count < 0xFFFF else { throw Failure.unsupported("too many entries") }
        try handle.seek(toOffset: UInt64(directoryOffset))
        let directory = try handle.read(upToCount: Int(directorySize)) ?? Data()
        guard directory.count == Int(directorySize), count == 0 || directory.uint32(at: 0) == centralSignature
        else { throw Failure.notAnArchive }

        let (time, day) = dosTimestamp(date)
        var offset = UInt64(directoryOffset)
        var added = Data()
        var central = Data()
        for entry in entries {
            let name = Data(entry.name.utf8)
            let crc = CRC32.checksum(entry.data)
            guard offset + 30 + UInt64(name.count) + UInt64(entry.data.count) < 0xFFFF_FFFF else {
                throw Failure.unsupported("an archive past 4 GiB")
            }
            var local = Data()
            local.append(le: localSignature)
            local.append(le: UInt16(10))  // version needed: stored
            local.append(le: UInt16(0x0800))  // UTF-8 name
            local.append(le: UInt16(0))  // stored
            local.append(le: time)
            local.append(le: day)
            local.append(le: crc)
            local.append(le: UInt32(entry.data.count))
            local.append(le: UInt32(entry.data.count))
            local.append(le: UInt16(name.count))
            local.append(le: UInt16(0))
            local.append(name)
            central.append(le: centralSignature)
            central.append(le: UInt16(0x031E))  // made by Unix, 3.0
            central.append(le: UInt16(10))
            central.append(le: UInt16(0x0800))
            central.append(le: UInt16(0))
            central.append(le: time)
            central.append(le: day)
            central.append(le: crc)
            central.append(le: UInt32(entry.data.count))
            central.append(le: UInt32(entry.data.count))
            central.append(le: UInt16(name.count))
            central.append(le: UInt16(0))  // extra
            central.append(le: UInt16(0))  // comment
            central.append(le: UInt16(0))  // disk
            central.append(le: UInt16(0))  // internal attributes
            central.append(le: UInt32(0o100644) << 16)  // a regular file, rw-r--r--
            central.append(le: UInt32(offset))
            central.append(name)
            added.append(local)
            added.append(entry.data)
            offset += UInt64(local.count + entry.data.count)
        }
        let newDirectorySize = UInt64(directory.count + central.count)
        guard offset + newDirectorySize < 0xFFFF_FFFF else { throw Failure.unsupported("an archive past 4 GiB") }
        var record = Data()
        record.append(le: endSignature)
        record.append(le: UInt16(0))
        record.append(le: UInt16(0))
        record.append(le: UInt16(Int(count) + entries.count))
        record.append(le: UInt16(Int(count) + entries.count))
        record.append(le: UInt32(newDirectorySize))
        record.append(le: UInt32(offset))
        record.append(le: UInt16(comment.count))
        record.append(comment)
        try handle.seek(toOffset: UInt64(directoryOffset))
        try handle.write(contentsOf: added)
        try handle.write(contentsOf: directory)
        try handle.write(contentsOf: central)
        try handle.write(contentsOf: record)
        try handle.truncate(atOffset: try handle.offset())
        try handle.synchronize()
    }

    private static func dosTimestamp(_ date: Date) -> (UInt16, UInt16) {
        let parts = Calendar(identifier: .gregorian).dateComponents(
            [.year, .month, .day, .hour, .minute, .second], from: date)
        let year = max(0, (parts.year ?? 1980) - 1980)
        let time = UInt16((parts.hour ?? 0) << 11 | (parts.minute ?? 0) << 5 | (parts.second ?? 0) / 2)
        let day = UInt16(year << 9 | (parts.month ?? 1) << 5 | (parts.day ?? 1))
        return (time, day)
    }
}

/// What Andarium adds to a bug report: the session records and the system's
/// exit and diagnostic reports MetricKit delivered, from `diagnostics`, with
/// the application's container paths taken out.
public enum BugReportAttachments {
    public static let sessionRecord = "session.txt"
    public static let previousSessionRecord = "previous-session.txt"
    public static let metricKitDirectory = "metrickit"

    public static func entries(from diagnostics: URL, containers: [String]) -> [BugReportArchive.Entry] {
        var entries: [BugReportArchive.Entry] = []
        let files: [(String, String)] = [
            (previousSessionRecord, "andarium/session-previous.txt"),
            (sessionRecord, "andarium/session-current.txt"),
        ]
        for (file, name) in files {
            if let text = try? String(contentsOf: diagnostics.appendingPathComponent(file), encoding: .utf8) {
                entries.append(.init(name: name, data: Data(DiagnosticRedaction.redact(text, containers: containers).utf8)))
            }
        }
        let metrics = diagnostics.appendingPathComponent(metricKitDirectory)
        let reports = (try? FileManager.default.contentsOfDirectory(atPath: metrics.path)) ?? []
        for report in reports.sorted() where report.hasSuffix(".json") {
            if let text = try? String(contentsOf: metrics.appendingPathComponent(report), encoding: .utf8) {
                entries.append(.init(
                    name: "andarium/metrickit/\(report)",
                    data: Data(DiagnosticRedaction.redact(text, containers: containers).utf8)))
            }
        }
        return entries
    }

    /// Adds the attachments to the report at `file`; a report that cannot take
    /// them is shared as Android made it.
    @discardableResult
    public static func attach(to file: URL, from diagnostics: URL, containers: [String]) -> Bool {
        let entries = entries(from: diagnostics, containers: containers)
        do {
            try BugReportArchive.append(entries, to: file)
            return true
        } catch {
            print("Andarium: the bug report goes without Andarium's records: \(error)")
            return false
        }
    }
}

/// The system's exit and diagnostic reports that MetricKit delivers, kept
/// beside the session records, newest `limit` of them.
public enum MetricKitReports {
    public static func keep(_ json: Data, named name: String, in diagnostics: URL, limit: Int = 30) {
        let directory = diagnostics.appendingPathComponent(BugReportAttachments.metricKitDirectory)
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try json.write(to: directory.appendingPathComponent(name), options: .atomic)
            let kept = try FileManager.default.contentsOfDirectory(atPath: directory.path)
                .filter { $0.hasSuffix(".json") }.sorted()
            for old in kept.dropLast(limit) {
                try? FileManager.default.removeItem(at: directory.appendingPathComponent(old))
            }
        } catch {
            print("Andarium: a MetricKit report was not kept: \(error.localizedDescription)")
        }
    }
}

/// The checksum a zip entry carries (IEEE 802.3, reflected).
enum CRC32 {
    private static let table: [UInt32] = (0..<256).map { index in
        var value = UInt32(index)
        for _ in 0..<8 { value = value & 1 != 0 ? 0xEDB8_8320 ^ (value >> 1) : value >> 1 }
        return value
    }

    static func checksum(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xFFFF_FFFF
        for byte in data { crc = table[Int((crc ^ UInt32(byte)) & 0xFF)] ^ (crc >> 8) }
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
