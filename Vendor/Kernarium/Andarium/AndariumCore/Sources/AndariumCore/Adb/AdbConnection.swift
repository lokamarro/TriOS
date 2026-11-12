// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What adbd said about itself in its CONNECT message.
public struct AdbBanner: Equatable, Sendable {
    /// The device's features, for example `shell_v2` and `abb_exec`.
    public var features: Set<String>
    /// The whole banner (`device::ro.product.name=...;features=...`).
    public var text: String

    init(payload: Data) {
        var bytes = payload
        while bytes.last == 0 { bytes.removeLast() }
        text = String(decoding: bytes, as: UTF8.self)
        var found = Set<String>()
        for field in text.split(separator: ";") where field.hasPrefix("features=") || field.contains("::features=") {
            let list = field.split(separator: "=", maxSplits: 1).last ?? ""
            found.formUnion(list.split(separator: ",").map(String.init))
        }
        features = found
    }
}

/// One connection to adbd: the handshake, then one stream at a time. Every
/// operation opens its own connection, so there is no multiplexing to share;
/// the stream's messages are the only ones a call waits for, and the late
/// replies of a stream already closed are ignored. Not thread-safe: one
/// operation thread uses it, and any other thread ends it through the
/// transport.
public final class AdbConnection {
    public let transport: any AdbTransport
    /// The largest payload either side sends: the smaller of the two limits.
    public private(set) var maxData = AdbWire.maxData
    public private(set) var banner = AdbBanner(payload: Data())
    private var nextLocalID: UInt32 = 1

    public init(transport: any AdbTransport) {
        self.transport = transport
    }

    // MARK: Messages

    public func send(_ message: AdbMessage) throws {
        try transport.writeAll(message.encoded())
    }

    public func receive() throws -> AdbMessage {
        let header = try AdbMessage.Header(transport.readExactly(AdbWire.headerSize))
        let payload = header.length > 0 ? try transport.readExactly(header.length) : Data()
        return AdbMessage(command: header.command, arg0: header.arg0, arg1: header.arg1, payload: payload)
    }

    // MARK: Handshake

    /// Sends CONNECT and answers AUTH. adbd with authorization on asks for a
    /// signature of a token; a second token means it did not accept the key.
    /// The host never offers its public key (`AUTH RSAPUBLICKEY`): adbd has
    /// the key from the boot, or the operation fails and nothing is asked of
    /// the person in Android.
    @discardableResult
    public func handshake(signer: (any AdbSigner)?) throws -> AdbBanner {
        var connect = Data(AdbWire.hostBanner.utf8)
        connect.append(0)
        try send(AdbMessage(
            command: AdbWire.cnxn, arg0: AdbWire.version, arg1: UInt32(AdbWire.maxData), payload: connect
        ))
        var signed = false
        while true {
            let message = try receive()
            switch message.command {
            case AdbWire.cnxn:
                banner = AdbBanner(payload: message.payload)
                maxData = max(1, min(AdbWire.maxData, Int(message.arg1)))
                return banner
            case AdbWire.auth where message.arg0 == AdbWire.authToken:
                guard let signer, !signed else { throw AdbError.keyRejected }
                try send(AdbMessage(
                    command: AdbWire.auth, arg0: AdbWire.authSignature, payload: signer.sign(token: message.payload)
                ))
                signed = true
            default:
                throw AdbError.protocolViolation("a message of \(String(format: "%08x", message.command)) in the handshake")
            }
        }
    }

    // MARK: Streams

    /// Opens a service, for example `shell,v2,raw:ls` or `sync:`. A service
    /// name may hold NUL characters (`abb_exec` separates its arguments with
    /// them). Throws `serviceRefused` when adbd closes the stream at once.
    public func open(_ service: String) throws -> AdbStream {
        let local = nextLocalID
        nextLocalID &+= 1
        var name = Data(service.utf8)
        name.append(0)
        try send(AdbMessage(command: AdbWire.open, arg0: local, payload: name))
        while true {
            let message = try receive()
            guard message.arg1 == local else { continue }
            switch message.command {
            case AdbWire.okay:
                return AdbStream(connection: self, localID: local, remoteID: message.arg0)
            case AdbWire.close:
                throw AdbError.serviceRefused(service)
            default:
                throw AdbError.protocolViolation("a message of \(String(format: "%08x", message.command)) answering an open")
            }
        }
    }
}

/// An open service on a connection.
public final class AdbStream {
    private unowned let connection: AdbConnection
    let localID: UInt32
    let remoteID: UInt32
    private var queued: [Data] = []
    public private(set) var isClosed = false

    init(connection: AdbConnection, localID: UInt32, remoteID: UInt32) {
        self.connection = connection
        self.localID = localID
        self.remoteID = remoteID
    }

    /// Sends `data` in payloads the peer accepts, one at a time: adbd answers
    /// each with OKAY before the next goes out. Output the service writes
    /// meanwhile is kept for `read`.
    public func write(_ data: Data) throws {
        var offset = data.startIndex
        while offset < data.endIndex {
            guard !isClosed else { throw AdbError.streamClosed }
            let end = min(offset + connection.maxData, data.endIndex)
            try connection.send(AdbMessage(
                command: AdbWire.write, arg0: localID, arg1: remoteID, payload: data[offset..<end]
            ))
            offset = end
            try awaitOkay()
        }
    }

    private func awaitOkay() throws {
        while true {
            let message = try connection.receive()
            guard message.arg1 == localID else { continue }
            switch message.command {
            case AdbWire.okay:
                return
            case AdbWire.write:
                try accept(message)
            case AdbWire.close:
                try peerClosed()
                throw AdbError.streamClosed
            default:
                continue
            }
        }
    }

    /// The next output of the service, or nil once it has closed the stream.
    public func read() throws -> Data? {
        while true {
            if !queued.isEmpty { return queued.removeFirst() }
            if isClosed { return nil }
            let message = try connection.receive()
            guard message.arg1 == localID else { continue }
            switch message.command {
            case AdbWire.write:
                try accept(message)
            case AdbWire.close:
                try peerClosed()
            default:
                continue
            }
        }
    }

    private func accept(_ message: AdbMessage) throws {
        try connection.send(AdbMessage(command: AdbWire.okay, arg0: localID, arg1: remoteID))
        if !message.payload.isEmpty { queued.append(message.payload) }
    }

    private func peerClosed() throws {
        guard !isClosed else { return }
        isClosed = true
        // Answer the close; the peer may already be gone.
        try? connection.send(AdbMessage(command: AdbWire.close, arg0: localID, arg1: remoteID))
    }

    /// Closes the stream from this side. Whatever the service had left to say
    /// is dropped; a late reply is ignored by later calls.
    public func close() {
        guard !isClosed else { return }
        isClosed = true
        try? connection.send(AdbMessage(command: AdbWire.close, arg0: localID, arg1: remoteID))
    }

    /// Reads to the end of the stream, `limit` bytes at most.
    public func readToEnd(limit: Int = 1 << 20) throws -> Data {
        var all = Data()
        while let chunk = try read() {
            all.append(chunk)
            if all.count > limit { throw AdbError.protocolViolation("more output than \(limit) bytes") }
        }
        return all
    }
}
