// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What `STAT` knows about a path on the device.
public struct AdbFileStat: Equatable, Sendable {
    public var mode: UInt32
    public var size: UInt64

    public var exists: Bool { mode != 0 }
}

/// The sync service (AOSP `adb/SYNC.TXT`): file transfer on a stream of
/// `id, length` requests. Only what reading one file needs: `STAT`, `RECV`
/// and `QUIT`.
public final class AdbSync {
    /// A `DATA` payload is at most this long.
    public static let maxChunk = 64 * 1024

    private let stream: AdbStream
    private let usesStatV2: Bool
    private var buffer = Data()

    /// Opens `sync:` on `connection`.
    public init(connection: AdbConnection) throws {
        stream = try connection.open("sync:")
        usesStatV2 = connection.banner.features.contains("stat_v2")
    }

    // MARK: Requests

    private func request(_ id: String, path: String) throws {
        let name = Data(path.utf8)
        var bytes = Data(id.utf8)
        bytes.appendLittleEndian(UInt32(name.count))
        bytes.append(name)
        try stream.write(bytes)
    }

    /// Reads exactly `count` bytes of the reply, which may span messages.
    private func take(_ count: Int) throws -> Data {
        while buffer.count < count {
            guard let more = try stream.read() else { throw AdbError.guestGone }
            buffer.append(more)
        }
        let taken = Data(buffer.prefix(count))
        buffer = Data(buffer.dropFirst(count))
        return taken
    }

    /// The size and mode of `path`. A path that is not there has mode 0.
    public func stat(_ path: String) throws -> AdbFileStat {
        try request(usesStatV2 ? "STA2" : "STAT", path: path)
        if usesStatV2 {
            // id, error, dev, ino, mode, nlink, uid, gid, size, atime, mtime, ctime
            let reply = try take(72)
            guard String(decoding: reply.prefix(4), as: UTF8.self) == "STA2" else {
                throw AdbError.protocolViolation("an unexpected reply to STA2")
            }
            guard reply.littleEndian32(at: 4) == 0 else { return AdbFileStat(mode: 0, size: 0) }
            return AdbFileStat(mode: reply.littleEndian32(at: 24), size: reply.littleEndian64(at: 40))
        }
        // id, mode, size, mtime
        let reply = try take(16)
        guard String(decoding: reply.prefix(4), as: UTF8.self) == "STAT" else {
            throw AdbError.protocolViolation("an unexpected reply to STAT")
        }
        return AdbFileStat(mode: reply.littleEndian32(at: 4), size: UInt64(reply.littleEndian32(at: 8)))
    }

    /// Reads `path`: `chunk` gets each `DATA` payload in order. A `FAIL` from
    /// the device is `AdbError.remote` with its text.
    public func receive(_ path: String, chunk: (Data) throws -> Void) throws {
        try request("RECV", path: path)
        while true {
            let header = try take(8)
            let id = String(decoding: header.prefix(4), as: UTF8.self)
            let length = Int(header.littleEndian32(at: 4))
            switch id {
            case "DATA":
                guard length <= AdbWire.maxData else {
                    throw AdbError.protocolViolation("a DATA chunk of \(length) bytes")
                }
                try chunk(try take(length))
            case "DONE":
                return
            case "FAIL":
                guard length <= 64 * 1024 else { throw AdbError.protocolViolation("a FAIL of \(length) bytes") }
                throw AdbError.remote(String(decoding: try take(length), as: UTF8.self))
            default:
                throw AdbError.protocolViolation("a sync reply \(id)")
            }
        }
    }

    /// Ends the session politely.
    public func quit() {
        var bytes = Data("QUIT".utf8)
        bytes.appendLittleEndian(UInt32(0))
        try? stream.write(bytes)
        stream.close()
    }
}
