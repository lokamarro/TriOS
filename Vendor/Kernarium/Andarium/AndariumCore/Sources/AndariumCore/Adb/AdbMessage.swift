// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The adb wire format (AOSP `adb/protocol.txt`): a 24-byte little-endian
/// header and an optional payload. Version 0x01000001 skips the payload
/// checksum.
public enum AdbWire {
    public static let headerSize = 24
    public static let version: UInt32 = 0x0100_0001
    /// The largest payload this host sends or accepts in one message.
    public static let maxData = 1 << 20

    public static let cnxn = fourCC("CNXN")
    public static let auth = fourCC("AUTH")
    public static let open = fourCC("OPEN")
    public static let okay = fourCC("OKAY")
    public static let close = fourCC("CLSE")
    public static let write = fourCC("WRTE")

    /// AUTH arg0 values.
    public static let authToken: UInt32 = 1
    public static let authSignature: UInt32 = 2
    public static let authRSAPublicKey: UInt32 = 3

    /// The features this host asks for. The device answers with the subset it
    /// has.
    public static let hostBanner = "host::features=shell_v2,cmd,stat_v2,abb_exec"

    /// A four-character tag as the little-endian number adb uses for commands.
    public static func fourCC(_ tag: String) -> UInt32 {
        var value: UInt32 = 0
        for (index, byte) in tag.utf8.prefix(4).enumerated() {
            value |= UInt32(byte) << (8 * UInt32(index))
        }
        return value
    }
}

public struct AdbMessage: Equatable, Sendable {
    public var command: UInt32
    public var arg0: UInt32
    public var arg1: UInt32
    public var payload: Data

    public init(command: UInt32, arg0: UInt32 = 0, arg1: UInt32 = 0, payload: Data = Data()) {
        self.command = command
        self.arg0 = arg0
        self.arg1 = arg1
        self.payload = payload
    }

    /// The header and payload as they go on the wire.
    public func encoded() -> Data {
        var data = Data(capacity: AdbWire.headerSize + payload.count)
        data.appendLittleEndian(command)
        data.appendLittleEndian(arg0)
        data.appendLittleEndian(arg1)
        data.appendLittleEndian(UInt32(payload.count))
        data.appendLittleEndian(UInt32(0))
        data.appendLittleEndian(~command)
        data.append(payload)
        return data
    }

    /// A header read from the wire; the payload follows it.
    public struct Header: Equatable, Sendable {
        public var command: UInt32
        public var arg0: UInt32
        public var arg1: UInt32
        public var length: Int

        /// Reads a 24-byte header. The magic must be the command's complement;
        /// the payload checksum is not checked, as for version 0x01000001.
        public init(_ bytes: Data, maxData: Int = AdbWire.maxData) throws {
            guard bytes.count == AdbWire.headerSize else {
                throw AdbError.protocolViolation("a header of \(bytes.count) bytes")
            }
            command = bytes.littleEndian32(at: 0)
            arg0 = bytes.littleEndian32(at: 4)
            arg1 = bytes.littleEndian32(at: 8)
            let length = bytes.littleEndian32(at: 12)
            guard bytes.littleEndian32(at: 20) == ~command else {
                throw AdbError.protocolViolation("a header with a bad magic")
            }
            guard length <= UInt32(maxData) else {
                throw AdbError.protocolViolation("a payload of \(length) bytes")
            }
            self.length = Int(length)
        }
    }
}

extension Data {
    /// Reads the number at `offset` from the start of the data, whatever the
    /// slice's own indices are.
    func littleEndian32(at offset: Int) -> UInt32 {
        var value: UInt32 = 0
        for index in 0..<4 {
            value |= UInt32(self[startIndex + offset + index]) << (8 * UInt32(index))
        }
        return value
    }

    func littleEndian64(at offset: Int) -> UInt64 {
        var value: UInt64 = 0
        for index in 0..<8 {
            value |= UInt64(self[startIndex + offset + index]) << (8 * UInt64(index))
        }
        return value
    }

    mutating func appendLittleEndian(_ value: UInt32) {
        for index in 0..<4 { append(UInt8(truncatingIfNeeded: value >> (8 * UInt32(index)))) }
    }

    mutating func appendLittleEndian(_ value: UInt64) {
        for index in 0..<8 { append(UInt8(truncatingIfNeeded: value >> (8 * UInt64(index)))) }
    }
}
