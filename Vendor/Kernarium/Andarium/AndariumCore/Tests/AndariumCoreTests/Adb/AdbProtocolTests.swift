// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
@preconcurrency import Security
import Testing

@testable import AndariumCore

// The adb client against a scripted adbd on a socket pair (FakeAdbd.swift): the
// codec, the handshake with authorization off, on and rejected, streams,
// shell v2, sync and the bug report parser. AdbOperationTests.swift adds the
// timeouts, on a clock the test moves, the session and a cancel from another
// thread, and DescriptorTransportTests.swift the end of a stream announced
// right after its last bytes.
struct AdbCodecTests {
    @Test func commandsAreTheLittleEndianTags() {
        #expect(AdbWire.cnxn == 0x4e58_4e43)
        #expect(AdbWire.auth == 0x4854_5541)
        #expect(AdbWire.open == 0x4e45_504f)
        #expect(AdbWire.okay == 0x5941_4b4f)
        #expect(AdbWire.close == 0x4553_4c43)
        #expect(AdbWire.write == 0x4554_5257)
        #expect(AdbWire.version == 0x0100_0001)
        #expect(AdbWire.maxData == 1_048_576)
    }

    @Test func aMessageIsA24ByteHeaderAndItsPayload() throws {
        let message = AdbMessage(command: AdbWire.write, arg0: 7, arg1: 9, payload: Data("hello".utf8))
        let bytes = message.encoded()
        #expect(bytes.count == 24 + 5)
        #expect(Array(bytes.prefix(4)) == Array("WRTE".utf8))
        let header = try AdbMessage.Header(bytes.prefix(24))
        #expect(header.command == AdbWire.write && header.arg0 == 7 && header.arg1 == 9 && header.length == 5)
        // The checksum is zero (version 0x01000001) and the magic is the complement.
        #expect(bytes.littleEndian32(at: 16) == 0)
        #expect(bytes.littleEndian32(at: 20) == ~AdbWire.write)
        #expect(bytes.suffix(5) == Data("hello".utf8))
    }

    @Test func aHeaderWithTheWrongMagicIsRefused() {
        var bytes = AdbMessage(command: AdbWire.okay).encoded()
        bytes[20] ^= 1
        #expect(throws: AdbError.protocolViolation("a header with a bad magic")) {
            try AdbMessage.Header(bytes)
        }
    }

    @Test func aPayloadAboveTheLimitIsRefused() {
        var bytes = AdbMessage(command: AdbWire.write).encoded()
        let tooMany = UInt32(AdbWire.maxData + 1)
        for index in 0..<4 { bytes[12 + index] = UInt8(truncatingIfNeeded: tooMany >> (8 * UInt32(index))) }
        #expect(throws: AdbError.self) { try AdbMessage.Header(bytes) }
    }

    @Test func aShortHeaderIsRefused() {
        #expect(throws: AdbError.self) { try AdbMessage.Header(Data(count: 23)) }
    }
}

@Suite(.timeLimit(.minutes(1)))
struct AdbHandshakeTests {
    private func ephemeralKey() throws -> (SecKey, SecKey) {
        let attributes: [String: Any] = [
            kSecAttrKeyType as String: kSecAttrKeyTypeRSA, kSecAttrKeySizeInBits as String: 2048,
        ]
        var error: Unmanaged<CFError>?
        let key = try #require(SecKeyCreateRandomKey(attributes as CFDictionary, &error))
        return (key, try #require(SecKeyCopyPublicKey(key)))
    }

    @Test func withAuthorizationOffTheDeviceAnswersConnect() throws {
        let adbd = FakeAdbd(auth: .off)
        defer { adbd.shutdown() }
        let connection = AdbConnection(transport: adbd.client)
        let banner = try connection.handshake(signer: nil)
        #expect(banner.features == ["shell_v2", "cmd", "stat_v2", "abb_exec"])
        #expect(banner.text.hasPrefix("device::ro.product.name=fake"))
        #expect(connection.maxData == AdbWire.maxData)
        #expect(adbd.tokensIssued == 0)
    }

    @Test func theDevicesSmallerMaxDataWins() throws {
        let adbd = FakeAdbd(auth: .off, maxData: 4096)
        defer { adbd.shutdown() }
        let connection = AdbConnection(transport: adbd.client)
        try connection.handshake(signer: nil)
        #expect(connection.maxData == 4096)
    }

    @Test func aTokenIsAnsweredWithASignatureTheKeyVerifies() throws {
        let (key, publicKey) = try ephemeralKey()
        let adbd = FakeAdbd(auth: .on { token, signature in
            SecKeyVerifySignature(publicKey, .rsaSignatureDigestPKCS1v15SHA1, token as CFData, signature as CFData, nil)
        })
        defer { adbd.shutdown() }
        let connection = AdbConnection(transport: adbd.client)
        try connection.handshake(signer: SecKeySigner(privateKey: key))
        #expect(adbd.tokensIssued == 1)
        #expect(!adbd.publicKeyWasOffered)
    }

    @Test func aSecondTokenMeansTheKeyWasRejectedAndTheKeyIsNeverOffered() throws {
        let (key, _) = try ephemeralKey()
        let adbd = FakeAdbd(auth: .on { _, _ in false })
        defer { adbd.shutdown() }
        let connection = AdbConnection(transport: adbd.client)
        #expect(throws: AdbError.keyRejected) {
            try connection.handshake(signer: SecKeySigner(privateKey: key))
        }
        #expect(adbd.tokensIssued == 2)
        #expect(!adbd.publicKeyWasOffered)
    }

    @Test func aTokenWithoutASignerIsARejection() {
        let adbd = FakeAdbd(auth: .on { _, _ in true })
        defer { adbd.shutdown() }
        let connection = AdbConnection(transport: adbd.client)
        #expect(throws: AdbError.keyRejected) { try connection.handshake(signer: nil) }
    }

    @Test func aSignatureIsPKCS1V15OverTheTokenAsASHA1Digest() throws {
        let (key, publicKey) = try ephemeralKey()
        let token = Data((0..<20).map { UInt8($0 &* 13 &+ 1) })
        let signature = try SecKeySigner(privateKey: key).sign(token: token)
        #expect(signature.count == 256)
        #expect(SecKeyVerifySignature(
            publicKey, .rsaSignatureDigestPKCS1v15SHA1, token as CFData, signature as CFData, nil))
        var other = token
        other[0] ^= 1
        #expect(!SecKeyVerifySignature(
            publicKey, .rsaSignatureDigestPKCS1v15SHA1, other as CFData, signature as CFData, nil))
    }

    @Test func aPeerThatEndsTheStreamIsGuestGone() {
        let adbd = FakeAdbd(auth: .off)
        adbd.shutdown()
        let connection = AdbConnection(transport: adbd.client)
        #expect(throws: AdbError.guestGone) { try connection.handshake(signer: nil) }
    }
}

@Suite(.timeLimit(.minutes(1)))
struct AdbStreamTests {
    private func connected(_ adbd: FakeAdbd) throws -> AdbConnection {
        let connection = AdbConnection(transport: adbd.client)
        try connection.handshake(signer: nil)
        return connection
    }

    @Test func aServiceTheDeviceLacksIsRefused() throws {
        let adbd = FakeAdbd()
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        #expect(throws: AdbError.serviceRefused("nothing:")) { try connection.open("nothing:") }
    }

    @Test func anOpenCarriesTheServiceAndItsTerminator() throws {
        let adbd = FakeAdbd(services: ["abb_exec:*": { _ in }])
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        _ = try connection.open("abb_exec:package\0install\0-t\0-S\0123")
        #expect(adbd.servicesOpened == ["abb_exec:package\0install\0-t\0-S\0123"])
    }

    @Test func dataGoesOutInPayloadsOfTheNegotiatedSizeEachAnsweredBeforeTheNext() throws {
        let adbd = FakeAdbd(maxData: 10, services: ["sink:": { stream in
            var lengths: [Int] = []
            var total = 0
            while total < 35, let chunk = try stream.receive() {
                lengths.append(chunk.count)
                total += chunk.count
            }
            try stream.send(Data(lengths.map(String.init).joined(separator: ",").utf8))
        }])
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let stream = try connection.open("sink:")
        try stream.write(Data(repeating: 0x41, count: 35))
        #expect(String(decoding: try #require(try stream.read()), as: UTF8.self) == "10,10,10,5")
        #expect(try stream.read() == nil)
        #expect(stream.isClosed)
    }

    @Test func outputThatArrivesWhileWritingIsKept() throws {
        let adbd = FakeAdbd(services: ["chatty:": { stream in
            try stream.send(Data("early".utf8))
            _ = try stream.receive(exactly: 4)
            try stream.send(Data("late".utf8))
        }])
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let stream = try connection.open("chatty:")
        try stream.write(Data("four".utf8))
        let all = try stream.readToEnd()
        #expect(String(decoding: all, as: UTF8.self) == "earlylate")
    }

    @Test func aServiceThatClosesEarlyLeavesItsOutputToRead() throws {
        let adbd = FakeAdbd(services: ["quit:": { stream in
            try stream.send(Data("Failure [INSTALL_FAILED_INVALID_APK]".utf8))
        }])
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let stream = try connection.open("quit:")
        var failure: AdbError?
        do {
            for _ in 0..<50 { try stream.write(Data(count: 1000)) }
        } catch let error as AdbError {
            failure = error
        }
        #expect(failure == .streamClosed)
        let said = try stream.readToEnd()
        #expect(String(decoding: said, as: UTF8.self) == "Failure [INSTALL_FAILED_INVALID_APK]")
    }

    @Test func aLateCloseOfAnEarlierStreamIsIgnored() throws {
        let adbd = FakeAdbd(services: ["one:": { _ in }, "two:": { stream in
            try stream.send(Data("two".utf8))
        }])
        defer { adbd.shutdown() }
        let connection = try connected(adbd)
        let first = try connection.open("one:")
        first.close()
        let second = try connection.open("two:")
        #expect(String(decoding: try second.readToEnd(), as: UTF8.self) == "two")
    }
}

struct ShellV2Tests {
    @Test func aPacketIsAnIDALengthAndData() {
        let packet = ShellPacket(id: ShellPacket.stdout, data: Data("ab".utf8))
        #expect(Array(packet.encoded()) == [1, 2, 0, 0, 0, 0x61, 0x62])
        #expect(ShellPacket.stdin == 0 && ShellPacket.stdout == 1 && ShellPacket.stderr == 2)
        #expect(ShellPacket.exit == 3 && ShellPacket.closeStdin == 4 && ShellPacket.windowSizeChange == 5)
    }

    @Test func packetsAreFoundWhereverTheMessagesSplitThem() throws {
        var bytes = Data()
        bytes.append(ShellPacket(id: ShellPacket.stdout, data: Data("hello ".utf8)).encoded())
        bytes.append(ShellPacket(id: ShellPacket.stderr, data: Data("oops".utf8)).encoded())
        bytes.append(ShellPacket(id: ShellPacket.exit, data: Data([3])).encoded())
        for size in [1, 2, 5, 7, bytes.count] {
            var decoder = ShellV2Decoder()
            var found: [ShellPacket] = []
            var offset = 0
            while offset < bytes.count {
                let end = min(offset + size, bytes.count)
                found += try decoder.feed(bytes[offset..<end])
                offset = end
            }
            #expect(found.map(\.id) == [1, 2, 3], "split every \(size) bytes")
            #expect(found.map { String(decoding: $0.data, as: UTF8.self) }.prefix(2) == ["hello ", "oops"])
            #expect(found[2].data == Data([3]))
            #expect(decoder.pendingBytes == 0)
        }
    }

    @Test func aPacketLargerThanAMessageIsRefused() {
        var bytes = Data([1])
        bytes.appendLittleEndian(UInt32(ShellV2Decoder.maxPacket + 1))
        var decoder = ShellV2Decoder()
        #expect(throws: AdbError.self) { try decoder.feed(bytes) }
    }

    @Test(.timeLimit(.minutes(1))) func aCommandReturnsItsOutputAndExitCode() throws {
        let adbd = FakeAdbd(services: ["shell,v2,raw:echo hi": { stream in
            var bytes = Data()
            bytes.append(ShellPacket(id: ShellPacket.stdout, data: Data("hi\n".utf8)).encoded())
            bytes.append(ShellPacket(id: ShellPacket.stderr, data: Data("warn\n".utf8)).encoded())
            bytes.append(ShellPacket(id: ShellPacket.exit, data: Data([7])).encoded())
            // One packet per message, and the exit packet split in two.
            try stream.send(bytes.prefix(8))
            try stream.send(bytes.dropFirst(8))
        }])
        defer { adbd.shutdown() }
        let connection = AdbConnection(transport: adbd.client)
        try connection.handshake(signer: nil)
        let result = try connection.runShell("echo hi")
        #expect(result.text == "hi\n")
        #expect(String(decoding: result.stderr, as: UTF8.self) == "warn\n")
        #expect(result.exitCode == 7)
    }
}

@Suite(.timeLimit(.minutes(1)))
struct AdbSyncTests {
    private func statV2(size: UInt64, mode: UInt32 = 0o100644, error: UInt32 = 0) -> Data {
        var reply = Data("STA2".utf8)
        reply.appendLittleEndian(error)
        reply.appendLittleEndian(UInt64(0))  // dev
        reply.appendLittleEndian(UInt64(0))  // ino
        reply.appendLittleEndian(mode)
        reply.appendLittleEndian(UInt32(1))  // nlink
        reply.appendLittleEndian(UInt32(2000))  // uid
        reply.appendLittleEndian(UInt32(2000))  // gid
        reply.appendLittleEndian(size)
        for _ in 0..<3 { reply.appendLittleEndian(UInt64(0)) }  // times
        return reply
    }

    private func request(_ stream: FakeStream) throws -> (id: String, path: String) {
        let header = try stream.receive(exactly: 8)
        let length = Int(header.littleEndian32(at: 4))
        let path = try stream.receive(exactly: length)
        return (String(decoding: header.prefix(4), as: UTF8.self), String(decoding: path, as: UTF8.self))
    }

    private func chunk(_ id: String, _ payload: Data) -> Data {
        var bytes = Data(id.utf8)
        bytes.appendLittleEndian(UInt32(payload.count))
        bytes.append(payload)
        return bytes
    }

    @Test func statV2ReadsSizeAndMode() throws {
        let adbd = FakeAdbd(services: ["sync:": { stream in
            let asked = try request(stream)
            #expect(asked.id == "STA2" && asked.path == "/some/report.zip")
            try stream.send(statV2(size: 5_000_000_000))
        }])
        defer { adbd.shutdown() }
        let connection = AdbConnection(transport: adbd.client)
        try connection.handshake(signer: nil)
        let sync = try AdbSync(connection: connection)
        let stat = try sync.stat("/some/report.zip")
        #expect(stat.exists && stat.size == 5_000_000_000 && stat.mode == 0o100644)
    }

    @Test func statWithoutV2UsesTheOlderReply() throws {
        let adbd = FakeAdbd(features: "shell_v2", services: ["sync:": { stream in
            let asked = try request(stream)
            #expect(asked.id == "STAT")
            var reply = Data("STAT".utf8)
            reply.appendLittleEndian(UInt32(0o100600))
            reply.appendLittleEndian(UInt32(1234))
            reply.appendLittleEndian(UInt32(0))
            try stream.send(reply)
        }])
        defer { adbd.shutdown() }
        let connection = AdbConnection(transport: adbd.client)
        try connection.handshake(signer: nil)
        let stat = try AdbSync(connection: connection).stat("/x")
        #expect(stat.size == 1234 && stat.mode == 0o100600)
    }

    @Test func aMissingPathHasModeZero() throws {
        let adbd = FakeAdbd(services: ["sync:": { stream in
            _ = try request(stream)
            try stream.send(statV2(size: 0, mode: 0, error: 2))
        }])
        defer { adbd.shutdown() }
        let connection = AdbConnection(transport: adbd.client)
        try connection.handshake(signer: nil)
        #expect(try !AdbSync(connection: connection).stat("/missing").exists)
    }

    @Test func receiveJoinsDataChunksThatSpanMessages() throws {
        let body = Data((0..<200_000).map { UInt8(truncatingIfNeeded: $0 &* 31) })
        let adbd = FakeAdbd(services: ["sync:": { stream in
            let asked = try request(stream)
            #expect(asked.id == "RECV" && asked.path == "/r.zip")
            var wire = Data()
            var offset = 0
            while offset < body.count {
                let end = min(offset + AdbSync.maxChunk, body.count)
                wire.append(chunk("DATA", body[offset..<end]))
                offset = end
            }
            wire.append(chunk("DONE", Data()).prefix(8))
            // Messages that cut headers and payloads at odd places.
            var position = 0
            while position < wire.count {
                let end = min(position + 7_001, wire.count)
                try stream.send(wire[position..<end])
                position = end
            }
        }])
        defer { adbd.shutdown() }
        let connection = AdbConnection(transport: adbd.client)
        try connection.handshake(signer: nil)
        let sync = try AdbSync(connection: connection)
        var received = Data()
        var sizes: [Int] = []
        try sync.receive("/r.zip") {
            received.append($0)
            sizes.append($0.count)
        }
        #expect(received == body)
        #expect(sizes.allSatisfy { $0 <= AdbSync.maxChunk })
    }

    @Test func aFailReplyIsTheDevicesOwnMessage() throws {
        let adbd = FakeAdbd(services: ["sync:": { stream in
            _ = try request(stream)
            try stream.send(chunk("FAIL", Data("open failed: Permission denied".utf8)))
        }])
        defer { adbd.shutdown() }
        let connection = AdbConnection(transport: adbd.client)
        try connection.handshake(signer: nil)
        let sync = try AdbSync(connection: connection)
        #expect(throws: AdbError.remote("open failed: Permission denied")) {
            try sync.receive("/secret") { _ in }
        }
    }

    @Test func aReplyOfAnotherKindIsAViolation() throws {
        let adbd = FakeAdbd(services: ["sync:": { stream in
            _ = try request(stream)
            try stream.send(chunk("WHAT", Data()))
        }])
        defer { adbd.shutdown() }
        let connection = AdbConnection(transport: adbd.client)
        try connection.handshake(signer: nil)
        let sync = try AdbSync(connection: connection)
        #expect(throws: AdbError.self) { try sync.receive("/x") { _ in } }
    }
}

struct BugreportzParserTests {
    @Test func linesAreRecognisedWhereverTheOutputIsCut() {
        let text = "BEGIN:/data/bugreports/b.zip\nPROGRESS:5/100\nPROGRESS:100/100\nOK:/data/bugreports/b.zip\n"
        let bytes = Data(text.utf8)
        for size in [1, 3, 10, bytes.count] {
            var parser = BugreportzParser()
            var lines: [BugreportzLine] = []
            var offset = 0
            while offset < bytes.count {
                let end = min(offset + size, bytes.count)
                lines += parser.feed(bytes[offset..<end])
                offset = end
            }
            #expect(lines == [
                .begin("/data/bugreports/b.zip"), .progress(5, 100), .progress(100, 100),
                .ok("/data/bugreports/b.zip"),
            ], "cut every \(size) bytes")
        }
    }

    @Test func aFailLineCarriesItsReason() {
        var parser = BugreportzParser()
        #expect(parser.feed(Data("FAIL:There is an existing bugreport in progress\n".utf8))
            == [.fail("There is an existing bugreport in progress")])
    }

    @Test func aLastLineWithoutANewlineIsFoundAtTheEnd() {
        var parser = BugreportzParser()
        #expect(parser.feed(Data("OK:/x.zip".utf8)).isEmpty)
        #expect(parser.finish() == [.ok("/x.zip")])
        #expect(parser.finish().isEmpty)
    }

    @Test func linesOfOtherKindsAndBadProgressAreKept() {
        var parser = BugreportzParser()
        let lines = parser.feed(Data("hello\r\nPROGRESS:x/y\nPROGRESS:3/0\n\n".utf8))
        #expect(lines == [.other("hello"), .other("PROGRESS:x/y"), .other("PROGRESS:3/0")])
    }
}
