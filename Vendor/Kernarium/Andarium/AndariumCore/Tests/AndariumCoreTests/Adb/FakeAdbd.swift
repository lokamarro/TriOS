// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

@testable import AndariumCore

/// A scripted adbd on the far end of a socket pair. It speaks the same wire
/// format as the client, one stream at a time, and runs on a thread of its
/// own; a service is a closure that talks through a `FakeStream`. Both ends
/// are shut down `limit` after it starts, so a test whose client or service
/// waits on something that never comes fails instead of hanging the run.
final class FakeAdbd: @unchecked Sendable {
    static let limit = DispatchTimeInterval.seconds(30)

    enum Auth {
        /// `ro.adb.secure=0`: CNXN answers CNXN.
        case off
        /// Asks for a signature; `verify(token, signature)` decides.
        case on(verify: @Sendable (Data, Data) -> Bool)
    }

    typealias Service = @Sendable (FakeStream) throws -> Void

    let client: DescriptorTransport
    private let server: DescriptorTransport
    private let auth: Auth
    private let features: String
    private let maxData: UInt32
    private let services: [String: Service]
    private let lock = NSLock()
    private var opened: [String] = []
    private var tokens = 0
    private var sawPublicKey = false
    private var finished = false
    private let expiry: DispatchWorkItem

    /// A service name ending in `*` matches by prefix.
    init(
        auth: Auth = .off, features: String = "shell_v2,cmd,stat_v2,abb_exec", maxData: UInt32 = 1 << 20,
        services: [String: Service] = [:]
    ) {
        var pair: [Int32] = [0, 0]
        precondition(socketpair(AF_UNIX, SOCK_STREAM, 0, &pair) == 0)
        let client = DescriptorTransport(descriptor: pair[0])
        let server = DescriptorTransport(descriptor: pair[1])
        self.client = client
        self.server = server
        expiry = DispatchWorkItem { [weak client, weak server] in
            client?.shutdown()
            server?.shutdown()
        }
        self.auth = auth
        self.features = features
        self.maxData = maxData
        self.services = services
        let thread = Thread { [self] in
            try? serve()
            server.shutdown()
            lock.withLock { finished = true }
        }
        thread.name = "fake-adbd"
        thread.start()
        DispatchQueue.global().asyncAfter(deadline: .now() + FakeAdbd.limit, execute: expiry)
    }

    var servicesOpened: [String] { lock.withLock { opened } }
    var tokensIssued: Int { lock.withLock { tokens } }
    var publicKeyWasOffered: Bool { lock.withLock { sawPublicKey } }

    func shutdown() {
        expiry.cancel()
        server.shutdown()
        client.shutdown()
    }

    // MARK: Wire

    func send(_ message: AdbMessage) throws {
        try server.writeAll(message.encoded())
    }

    func receive() throws -> AdbMessage {
        let header = try AdbMessage.Header(server.readExactly(AdbWire.headerSize))
        let payload = header.length > 0 ? try server.readExactly(header.length) : Data()
        return AdbMessage(command: header.command, arg0: header.arg0, arg1: header.arg1, payload: payload)
    }

    private func serve() throws {
        let connect = try receive()
        guard connect.command == AdbWire.cnxn else { return }
        try authenticate()
        try send(AdbMessage(
            command: AdbWire.cnxn, arg0: AdbWire.version, arg1: maxData,
            payload: Data("device::ro.product.name=fake;features=\(features)\0".utf8)
        ))
        var nextRemote: UInt32 = 100
        while true {
            let message = try receive()
            guard message.command == AdbWire.open else { continue }
            var name = message.payload
            if name.last == 0 { name.removeLast() }
            let service = String(decoding: name, as: UTF8.self)
            lock.withLock { opened.append(service) }
            guard let handler = find(service) else {
                try send(AdbMessage(command: AdbWire.close, arg0: 0, arg1: message.arg0))
                continue
            }
            let remote = nextRemote
            nextRemote += 1
            try send(AdbMessage(command: AdbWire.okay, arg0: remote, arg1: message.arg0))
            let stream = FakeStream(adbd: self, service: service, local: remote, peer: message.arg0)
            try handler(stream)
            stream.finish()
        }
    }

    private func find(_ service: String) -> Service? {
        if let exact = services[service] { return exact }
        return services.first { $0.key.hasSuffix("*") && service.hasPrefix(String($0.key.dropLast())) }?.value
    }

    private func authenticate() throws {
        guard case let .on(verify) = auth else { return }
        var token = Data((0..<20).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ 3) })
        for attempt in 0..<2 {
            token[0] = UInt8(attempt)
            lock.withLock { tokens += 1 }
            try send(AdbMessage(command: AdbWire.auth, arg0: AdbWire.authToken, payload: token))
            let reply = try receive()
            guard reply.command == AdbWire.auth else { throw AdbError.protocolViolation("not an AUTH") }
            if reply.arg0 == AdbWire.authRSAPublicKey {
                lock.withLock { sawPublicKey = true }
                throw AdbError.keyRejected
            }
            if reply.arg0 == AdbWire.authSignature, verify(token, reply.payload) { return }
        }
        // Not accepted: leave the client waiting for its answer.
        _ = try? receive()
        throw AdbError.keyRejected
    }
}

/// One stream of the fake adbd, as the service sees it.
final class FakeStream: @unchecked Sendable {
    let service: String
    private unowned let adbd: FakeAdbd
    private let local: UInt32
    private let peer: UInt32
    private(set) var closed = false
    private var buffer = Data()
    private var inbox: [Data] = []

    init(adbd: FakeAdbd, service: String, local: UInt32, peer: UInt32) {
        self.adbd = adbd
        self.service = service
        self.local = local
        self.peer = peer
    }

    /// Sends one WRTE and waits for its OKAY. Data the client writes meanwhile
    /// is acknowledged and kept for `receive`.
    func send(_ data: Data) throws {
        try adbd.send(AdbMessage(command: AdbWire.write, arg0: local, arg1: peer, payload: data))
        while true {
            let message = try adbd.receive()
            if message.command == AdbWire.okay { return }
            if message.command == AdbWire.write {
                try adbd.send(AdbMessage(command: AdbWire.okay, arg0: local, arg1: peer))
                inbox.append(message.payload)
            }
            if message.command == AdbWire.close {
                closed = true
                throw AdbError.streamClosed
            }
        }
    }

    /// The next WRTE from the client, acknowledged; nil when the client closed.
    func receive() throws -> Data? {
        if !inbox.isEmpty { return inbox.removeFirst() }
        while true {
            let message = try adbd.receive()
            if message.command == AdbWire.write {
                try adbd.send(AdbMessage(command: AdbWire.okay, arg0: local, arg1: peer))
                return message.payload
            }
            if message.command == AdbWire.close {
                closed = true
                return nil
            }
        }
    }

    /// Reads exactly `count` bytes from the client.
    func receive(exactly count: Int) throws -> Data {
        while buffer.count < count {
            guard let more = try receive() else { throw AdbError.guestGone }
            buffer.append(more)
        }
        let taken = Data(buffer.prefix(count))
        buffer = Data(buffer.dropFirst(count))
        return taken
    }

    func close() throws {
        guard !closed else { return }
        closed = true
        try adbd.send(AdbMessage(command: AdbWire.close, arg0: local, arg1: peer))
    }

    fileprivate func finish() {
        try? close()
    }
}

/// A transport that does nothing but count its shutdowns, for the watchdog.
final class CountingTransport: AdbTransport, @unchecked Sendable {
    private let lock = NSLock()
    private var count = 0

    var shutdowns: Int { lock.withLock { count } }

    func readExactly(_ count: Int) throws -> Data { throw AdbError.guestGone }
    func writeAll(_ data: Data) throws {}
    func shutdown() { lock.withLock { count += 1 } }
}

/// A connector that fails a number of times, then hands out a transport.
final class ScriptedConnector: AdbConnector, @unchecked Sendable {
    private let lock = NSLock()
    private var failures: [AdbError]
    private let transport: () -> any AdbTransport
    private var attempts = 0

    init(failures: [AdbError] = [], transport: @escaping () -> any AdbTransport) {
        self.failures = failures
        self.transport = transport
    }

    var attemptCount: Int { lock.withLock { attempts } }

    func connect() throws -> any AdbTransport {
        let failure = lock.withLock { () -> AdbError? in
            attempts += 1
            return failures.isEmpty ? nil : failures.removeFirst()
        }
        if let failure { throw failure }
        return transport()
    }
}

/// Collects a bug report's bytes in memory.
final class MemorySink: AdbSink {
    private(set) var data = Data()
    func write(_ data: Data) throws { self.data.append(data) }
}
