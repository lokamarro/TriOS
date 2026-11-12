// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// One packet of adb's shell protocol v2 (AOSP `adb/shell_protocol.h`): an id
/// byte, a 32-bit little-endian length and the data. The protocol carries a
/// shell command's stdout, stderr and exit code on one stream.
public struct ShellPacket: Equatable, Sendable {
    public static let stdin: UInt8 = 0
    public static let stdout: UInt8 = 1
    public static let stderr: UInt8 = 2
    public static let exit: UInt8 = 3
    public static let closeStdin: UInt8 = 4
    public static let windowSizeChange: UInt8 = 5

    public var id: UInt8
    public var data: Data

    public init(id: UInt8, data: Data) {
        self.id = id
        self.data = data
    }

    public func encoded() -> Data {
        var bytes = Data([id])
        bytes.appendLittleEndian(UInt32(data.count))
        bytes.append(data)
        return bytes
    }
}

/// Splits a byte stream into shell packets. A packet may span several
/// messages, and a message may hold several packets.
public struct ShellV2Decoder: Sendable {
    /// No packet is larger than a message's payload.
    public static let maxPacket = AdbWire.maxData

    private var buffer = Data()

    public init() {}

    public mutating func feed(_ data: Data) throws -> [ShellPacket] {
        buffer.append(data)
        var packets: [ShellPacket] = []
        var offset = buffer.startIndex
        while buffer.endIndex - offset >= 5 {
            let length = Int(buffer.littleEndian32(at: offset - buffer.startIndex + 1))
            guard length <= Self.maxPacket else {
                throw AdbError.protocolViolation("a shell packet of \(length) bytes")
            }
            guard buffer.endIndex - offset >= 5 + length else { break }
            packets.append(ShellPacket(id: buffer[offset], data: Data(buffer[(offset + 5)..<(offset + 5 + length)])))
            offset += 5 + length
        }
        buffer = Data(buffer[offset...])
        return packets
    }

    /// Bytes of a packet that has not completely arrived.
    public var pendingBytes: Int { buffer.count }
}

/// What a shell command left behind.
public struct ShellResult: Equatable, Sendable {
    public var stdout = Data()
    public var stderr = Data()
    /// The command's exit status; nil when the stream ended without one.
    public var exitCode: Int?

    public init(stdout: Data = Data(), stderr: Data = Data(), exitCode: Int? = nil) {
        self.stdout = stdout
        self.stderr = stderr
        self.exitCode = exitCode
    }

    public var text: String { String(decoding: stdout, as: UTF8.self) }
}

extension AdbConnection {
    /// The service string for a shell command with the v2 protocol and no
    /// terminal: separate stdout and stderr, no line-ending rewriting.
    public static func shellService(_ command: String) -> String {
        "shell,v2,raw:\(command)"
    }

    /// Runs a command in Android's shell and returns its output once it has
    /// exited. `onOutput` sees stdout as it arrives. Output beyond `limit`
    /// bytes of either stream is an error: this is for short commands.
    @discardableResult
    public func runShell(
        _ command: String, limit: Int = 1 << 20, onOutput: ((Data) -> Void)? = nil
    ) throws -> ShellResult {
        let stream = try open(Self.shellService(command))
        defer { stream.close() }
        var decoder = ShellV2Decoder()
        var result = ShellResult()
        while let chunk = try stream.read() {
            for packet in try decoder.feed(chunk) {
                switch packet.id {
                case ShellPacket.stdout:
                    result.stdout.append(packet.data)
                    onOutput?(packet.data)
                case ShellPacket.stderr:
                    result.stderr.append(packet.data)
                case ShellPacket.exit:
                    result.exitCode = packet.data.first.map(Int.init)
                default:
                    break
                }
            }
            if result.stdout.count > limit || result.stderr.count > limit {
                throw AdbError.protocolViolation("more output than \(limit) bytes")
            }
        }
        return result
    }
}
