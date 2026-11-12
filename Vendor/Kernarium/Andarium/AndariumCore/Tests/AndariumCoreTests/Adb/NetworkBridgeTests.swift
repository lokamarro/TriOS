// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing

@testable import AndariumCore

/// A guest on the far end of a socket pair for every stream the bridge opens:
/// `handler` talks through its end on a thread of its own.
final class LoopbackGuest: GuestStreamOpener, @unchecked Sendable {
    typealias Handler = @Sendable (Int32) -> Void

    private let lock = NSLock()
    private var opened = 0
    private let handler: Handler
    private let failure: Int32?

    /// `failure` is the error a stream that cannot be opened throws.
    init(failure: Int32? = nil, handler: @escaping Handler = { _ in }) {
        self.handler = handler
        self.failure = failure
    }

    var streamsOpened: Int { lock.withLock { opened } }

    func openStream() throws -> Int32 {
        if let failure { throw AdbError.io(failure) }
        var pair: [Int32] = [0, 0]
        guard socketpair(AF_UNIX, SOCK_STREAM, 0, &pair) == 0 else { throw AdbError.io(errno) }
        lock.withLock { opened += 1 }
        let guestEnd = pair[1]
        // A write to a stream the bridge ended must fail, not kill the test,
        // and one the bridge never takes fails too.
        var on: Int32 = 1
        _ = setsockopt(guestEnd, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        var timeout = timeval(tv_sec: Int(LoopbackGuest.waitSeconds), tv_usec: 0)
        _ = setsockopt(guestEnd, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        let handler = handler
        let thread = Thread {
            handler(guestEnd)
            Darwin.close(guestEnd)
        }
        thread.name = "loopback-guest"
        thread.start()
        return pair[0]
    }

    /// The longest a fake guest or a computer waits for the other side.
    static let waitSeconds: Int32 = 30

    /// Waits until `fd` can be read or has ended; false when `waitSeconds`
    /// pass first. A blocking `read` on a socket pair can miss the end its
    /// peer announces with `shutdown(SHUT_WR)` on Darwin, which would hang a
    /// test now and then.
    static func waitReadable(_ fd: Int32) -> Bool {
        var entry = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
        while true {
            let ready = poll(&entry, 1, waitSeconds * 1000)
            if ready >= 0 { return ready > 0 }
            if errno != EINTR { return true }
        }
    }

    /// Reads until the stream ends, or nothing comes for `waitSeconds`, and
    /// returns what arrived.
    static func readAll(_ fd: Int32) -> Data {
        var data = Data()
        var buffer = [UInt8](repeating: 0, count: 64 * 1024)
        while true {
            guard waitReadable(fd) else { return data }
            let got = Darwin.read(fd, &buffer, buffer.count)
            if got > 0 {
                data.append(buffer, count: got)
            } else if got == 0 || errno != EINTR {
                return data
            }
        }
    }

    static func writeAll(_ fd: Int32, _ data: Data) -> Bool {
        var done = 0
        while done < data.count {
            let wrote = data.withUnsafeBytes { Darwin.write(fd, $0.baseAddress! + done, data.count - done) }
            if wrote > 0 {
                done += wrote
            } else if wrote < 0 && errno == EINTR {
                continue
            } else {
                return false
            }
        }
        return true
    }

    /// Writes back what arrives until the stream ends.
    static let echo: Handler = { fd in
        var buffer = [UInt8](repeating: 0, count: 64 * 1024)
        while true {
            guard waitReadable(fd) else { return }
            let got = Darwin.read(fd, &buffer, buffer.count)
            if got > 0 {
                if !writeAll(fd, Data(buffer[0..<got])) { return }
            } else if got == 0 || errno != EINTR {
                return
            }
        }
    }
}

/// A computer on the network: a TCP client of the bridge.
final class Computer: @unchecked Sendable {
    let fd: Int32
    private var closed = false

    /// `afterRefusal` runs when a connect failed and its socket is closed,
    /// before the error is thrown; whatever opens there takes the number the
    /// computer let go of, as a parallel test's socket can.
    init(port: UInt16, afterRefusal: (Int32) -> Void = { _ in }) throws {
        // The socket is the computer's only once it is connected: an init
        // that throws after every property is set runs `deinit`, which would
        // close the number again, whoever holds it by then.
        fd = try Computer.connect(port: port, afterRefusal: afterRefusal)
        var on: Int32 = 1
        _ = setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        // A test never waits for a silent or a stalled socket for long.
        var timeout = timeval(tv_sec: Int(LoopbackGuest.waitSeconds), tv_usec: 0)
        _ = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        _ = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
    }

    private static func connect(port: UInt16, afterRefusal: (Int32) -> Void) throws -> Int32 {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw AdbError.io(errno) }
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian
        address.sin_addr = in_addr(s_addr: UInt32(0x7F00_0001).bigEndian)
        let connected = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard connected == 0 else {
            let code = errno
            Darwin.close(fd)
            afterRefusal(fd)
            throw AdbError.io(code)
        }
        return fd
    }

    deinit { if !closed { Darwin.close(fd) } }

    func send(_ data: Data) -> Bool { LoopbackGuest.writeAll(fd, data) }

    /// Reads exactly `count` bytes, or fewer when the stream ends first.
    func receive(count: Int) -> Data {
        var data = Data()
        var buffer = [UInt8](repeating: 0, count: 64 * 1024)
        while data.count < count {
            let got = Darwin.read(fd, &buffer, min(buffer.count, count - data.count))
            if got > 0 {
                data.append(buffer, count: got)
            } else if got == 0 || errno != EINTR {
                break
            }
        }
        return data
    }

    /// Reads and drops bytes until `count` have arrived or the stream ends;
    /// returns how many arrived.
    func drain(count: Int) -> Int {
        var total = 0
        var buffer = [UInt8](repeating: 0, count: 256 * 1024)
        while total < count {
            let got = Darwin.read(fd, &buffer, min(buffer.count, count - total))
            if got > 0 {
                total += got
            } else if got == 0 || errno != EINTR {
                break
            }
        }
        return total
    }

    func receiveAll() -> Data { LoopbackGuest.readAll(fd) }

    func finishSending() { _ = Darwin.shutdown(fd, SHUT_WR) }

    /// Closes the socket, as a computer that goes away does. (`shutdown` would
    /// do nothing once the other end has closed its own.)
    func close() {
        guard !closed else { return }
        closed = true
        Darwin.close(fd)
    }

    /// Whether the other end has ended the stream: a read of nothing, or a
    /// reset. A read that timed out is not an end.
    func sawEnd() -> Bool {
        var byte: UInt8 = 0
        let got = Darwin.read(fd, &byte, 1)
        return got == 0 || (got < 0 && (errno == ECONNRESET || errno == EPIPE))
    }
}

/// Bytes with a pattern that tells a lost or doubled block.
func patternData(_ count: Int, seed: UInt8 = 1) -> Data {
    var data = Data(count: count)
    var state = UInt32(seed) &* 2_654_435_761 &+ 1
    data.withUnsafeMutableBytes { buffer in
        for index in 0..<count {
            state = state &* 1_664_525 &+ 1_013_904_223
            buffer[index] = UInt8(truncatingIfNeeded: state >> 24)
        }
    }
    return data
}

func eventually(_ seconds: Double = 10, _ condition: () -> Bool) -> Bool {
    let deadline = Date().addingTimeInterval(seconds)
    while Date() < deadline {
        if condition() { return true }
        usleep(5_000)
    }
    return condition()
}

/// Whether `counter` stops below `limit` within `seconds`: it has not moved
/// for `quiet` seconds. A side that is held back stops once the sockets in
/// between are full, and how soon that is depends on the host's load.
func settles(_ counter: ByteCounter, below limit: Int, within seconds: Double = 30, quiet: Double = 0.3) -> Bool {
    let deadline = Date().addingTimeInterval(seconds)
    var last = counter.value
    var since = Date()
    while last < limit, Date() < deadline {
        usleep(10_000)
        let now = counter.value
        if now != last {
            last = now
            since = Date()
        } else if Date().timeIntervalSince(since) >= quiet {
            return true
        }
    }
    return false
}

/// A loopback port nothing listens on: a listener's, closed again. (A port
/// that is bound and not listening is not refused; the connect times out.)
func refusedPort() throws -> UInt16 {
    let fd = socket(AF_INET, SOCK_STREAM, 0)
    guard fd >= 0 else { throw AdbError.io(errno) }
    defer { Darwin.close(fd) }
    var address = sockaddr_in()
    address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
    address.sin_family = sa_family_t(AF_INET)
    address.sin_addr = in_addr(s_addr: UInt32(0x7F00_0001).bigEndian)
    var length = socklen_t(MemoryLayout<sockaddr_in>.size)
    let ready = withUnsafeMutablePointer(to: &address) { pointer in
        pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            Darwin.bind(fd, $0, length) == 0 && listen(fd, 1) == 0 && getsockname(fd, $0, &length) == 0
        }
    }
    guard ready else { throw AdbError.io(errno) }
    return UInt16(bigEndian: address.sin_port)
}

/// What a descriptor names, to tell whether a number still names it.
struct FileIdentity: Equatable {
    let device: dev_t
    let inode: ino_t

    init?(_ fd: Int32) {
        var info = stat()
        guard fstat(fd, &info) == 0 else { return nil }
        device = info.st_dev
        inode = info.st_ino
    }
}

/// The listener of ADB over network on loopback against a fake guest: bytes
/// both ways, backpressure each way, half close, a failed stream, the limit of
/// computers, stopping and a port in use.
@Suite(.timeLimit(.minutes(1)))
struct AdbNetworkBridgeTests {
    private func started(
        _ guest: LoopbackGuest, maxPeers: Int = 8
    ) throws -> (AdbNetworkBridge, UInt16) {
        let bridge = AdbNetworkBridge(port: 0, maxPeers: maxPeers, opener: guest)
        return (bridge, try bridge.start())
    }

    @Test func itCarriesBytesBothWaysAndTheEndOfEachStream() throws {
        let guest = LoopbackGuest(handler: LoopbackGuest.echo)
        let (bridge, port) = try started(guest)
        defer { bridge.stop() }
        let computer = try Computer(port: port)
        let data = patternData(1 << 20)
        let sender = Thread { _ = computer.send(data) }
        sender.start()
        #expect(computer.receive(count: data.count) == data)
        computer.finishSending()
        // The guest sees the end, ends its own, and the computer sees that.
        #expect(computer.sawEnd())
        #expect(eventually { bridge.peerCount == 0 })
        #expect(guest.streamsOpened == 1)
    }

    @Test func aGuestThatIsSlowHoldsTheComputerBack() throws {
        let release = DispatchSemaphore(value: 0)
        let received = ByteCounter()
        let guest = LoopbackGuest { fd in
            _ = release.wait(timeout: .now() + 60)
            received.add(LoopbackGuest.readAll(fd).count)
        }
        let (bridge, port) = try started(guest)
        defer { bridge.stop() }
        let computer = try Computer(port: port)
        let total = 128 << 20
        let written = ByteCounter()
        let writer = Thread {
            let block = patternData(256 * 1024)
            var sent = 0
            while sent < total {
                guard computer.send(block) else { return }
                sent += block.count
                written.set(sent)
            }
            computer.finishSending()
        }
        writer.start()
        // The sockets between them hold a few megabytes, not a hundred and
        // twenty-eight.
        #expect(settles(written, below: total / 2))
        release.signal()
        #expect(eventually(30) { received.value == total })
        #expect(written.value == total)
    }

    @Test func aSlowComputerHoldsTheGuestBack() throws {
        let sent = ByteCounter()
        let total = 128 << 20
        let drained = DispatchSemaphore(value: 0)
        let finished = DispatchSemaphore(value: 0)
        let guest = LoopbackGuest { fd in
            defer { finished.signal() }
            let block = patternData(256 * 1024)
            var done = 0
            while done < total {
                guard LoopbackGuest.writeAll(fd, block) else { return }
                done += block.count
                // The computer can read the last block before this thread is
                // back from writing it, and a busy host keeps the thread away
                // longer: it waits here until the computer has it all, and the
                // test reads the count only once the guest is done.
                if done == total { _ = drained.wait(timeout: .now() + 30) }
                sent.set(done)
            }
        }
        let (bridge, port) = try started(guest)
        defer { bridge.stop() }
        let computer = try Computer(port: port)
        #expect(settles(sent, below: total / 2))
        #expect(computer.drain(count: total) == total)
        drained.signal()
        #expect(finished.wait(timeout: .now() + 30) == .success)
        #expect(sent.value == total)
    }

    @Test func theGuestEndingFirstEndsTheComputersStream() throws {
        let guest = LoopbackGuest { fd in
            _ = LoopbackGuest.writeAll(fd, Data("hello".utf8))
        }
        let (bridge, port) = try started(guest)
        defer { bridge.stop() }
        let computer = try Computer(port: port)
        #expect(computer.receiveAll() == Data("hello".utf8))
        // The computer closes too, which frees the connection.
        computer.close()
        #expect(eventually { bridge.peerCount == 0 })
    }

    @Test func theComputerEndingFirstEndsTheGuestsStream() throws {
        let seen = DataBox()
        let guest = LoopbackGuest { fd in seen.set(LoopbackGuest.readAll(fd)) }
        let (bridge, port) = try started(guest)
        defer { bridge.stop() }
        let computer = try Computer(port: port)
        #expect(computer.send(Data("bye".utf8)))
        computer.close()
        #expect(eventually { seen.value == Data("bye".utf8) })
        #expect(eventually { bridge.peerCount == 0 })
    }

    @Test func aHalfClosedComputerStillGetsTheReply() throws {
        let guest = LoopbackGuest { fd in
            let request = LoopbackGuest.readAll(fd)
            _ = LoopbackGuest.writeAll(fd, Data("re:".utf8) + request)
        }
        let (bridge, port) = try started(guest)
        defer { bridge.stop() }
        let computer = try Computer(port: port)
        #expect(computer.send(Data("ask".utf8)))
        computer.finishSending()
        #expect(computer.receiveAll() == Data("re:ask".utf8))
    }

    @Test func aGuestThatCannotBeReachedClosesTheComputer() throws {
        let guest = LoopbackGuest(failure: ECONNREFUSED)
        let (bridge, port) = try started(guest)
        defer { bridge.stop() }
        let computer = try Computer(port: port)
        #expect(computer.sawEnd())
        #expect(eventually { bridge.peerCount == 0 })
    }

    @Test func itServesOnlyAsManyComputersAsItIsGiven() throws {
        let guest = LoopbackGuest(handler: LoopbackGuest.echo)
        let (bridge, port) = try started(guest, maxPeers: 2)
        defer { bridge.stop() }
        let first = try Computer(port: port)
        let second = try Computer(port: port)
        #expect(first.send(Data("1".utf8)) && first.receive(count: 1) == Data("1".utf8))
        #expect(second.send(Data("2".utf8)) && second.receive(count: 1) == Data("2".utf8))
        let third = try Computer(port: port)
        #expect(third.sawEnd())
        #expect(bridge.peerCount == 2)
        // A place is free again once a computer leaves.
        first.close()
        #expect(eventually { bridge.peerCount == 1 })
        let fourth = try Computer(port: port)
        #expect(fourth.send(Data("4".utf8)) && fourth.receive(count: 1) == Data("4".utf8))
        #expect(guest.streamsOpened == 3)
    }

    @Test func manyComputersAtOnceKeepTheirOwnBytes() throws {
        let guest = LoopbackGuest(handler: LoopbackGuest.echo)
        let (bridge, port) = try started(guest)
        defer { bridge.stop() }
        let group = DispatchGroup()
        let failures = ByteCounter()
        for index in 0..<8 {
            group.enter()
            let thread = Thread {
                defer { group.leave() }
                guard let computer = try? Computer(port: port) else {
                    failures.add(1)
                    return
                }
                let data = patternData(300_000, seed: UInt8(index + 1))
                let sender = Thread { _ = computer.send(data) }
                sender.start()
                if computer.receive(count: data.count) != data { failures.add(1) }
            }
            thread.start()
        }
        #expect(group.wait(timeout: .now() + 30) == .success)
        #expect(failures.value == 0)
    }

    @Test func stoppingEndsEveryConnectionAndFreesThePort() throws {
        let ended = DispatchSemaphore(value: 0)
        let guest = LoopbackGuest { fd in
            _ = LoopbackGuest.readAll(fd)
            ended.signal()
        }
        let (bridge, port) = try started(guest)
        let first = try Computer(port: port)
        let second = try Computer(port: port)
        #expect(eventually { bridge.peerCount == 2 })
        bridge.stop()
        #expect(first.sawEnd() && second.sawEnd())
        #expect(ended.wait(timeout: .now() + 10) == .success)
        #expect(ended.wait(timeout: .now() + 10) == .success)
        #expect(bridge.waitUntilIdle(timeout: .seconds(10)))
        #expect(!bridge.isRunning && bridge.peerCount == 0)
        #expect(throws: AdbError.io(ECONNREFUSED)) { try Computer(port: port) }
        // The same port starts again.
        let again = AdbNetworkBridge(port: port, opener: guest)
        #expect(try again.start() == port)
        again.stop()
    }

    /// The harness: a computer that is refused closes its socket once. A
    /// second close would close whatever took the number meanwhile, such as a
    /// parallel test's adbd connection, whose waiting handshake never ends.
    @Test func aRefusedComputerLeavesTheNextSocketAlone() throws {
        let port = try refusedPort()
        var proved = 0
        for _ in 0..<10 {
            var victim: [(end: Int32, identity: FileIdentity?)] = []
            var taken: Int32?
            #expect(throws: AdbError.io(ECONNREFUSED)) {
                try Computer(port: port) { freed in
                    var pair: [Int32] = [-1, -1]
                    guard socketpair(AF_UNIX, SOCK_STREAM, 0, &pair) == 0 else { return }
                    victim = pair.map { ($0, FileIdentity($0)) }
                    // Another test may have taken the number first.
                    taken = pair.first { $0 == freed }
                }
            }
            if let taken, let opened = victim.first(where: { $0.end == taken }) {
                proved += 1
                #expect(FileIdentity(taken) == opened.identity, "the computer closed the socket opened after it")
            }
            for opened in victim where opened.identity != nil && FileIdentity(opened.end) == opened.identity {
                Darwin.close(opened.end)
            }
        }
        #expect(proved > 0)
    }

    @Test func stoppingEndsConnectionsThatAreBusyInBothDirections() throws {
        let guestEnded = DispatchSemaphore(value: 0)
        let release = DispatchSemaphore(value: 0)
        // The guest writes without end and reads nothing, so the bridge's
        // two threads are both blocked in a write when the stop comes.
        let guest = LoopbackGuest { fd in
            let block = patternData(64 * 1024)
            while LoopbackGuest.writeAll(fd, block) {}
            guestEnded.signal()
            _ = release.wait(timeout: .now() + 60)
        }
        let (bridge, port) = try started(guest)
        let computers = try (0..<3).map { _ in try Computer(port: port) }
        let writers = computers.map { computer in
            Thread {
                let block = patternData(64 * 1024, seed: 9)
                while computer.send(block) {}
            }
        }
        writers.forEach { $0.start() }
        #expect(eventually { bridge.peerCount == 3 })
        Thread.sleep(forTimeInterval: 0.3)
        let began = Date()
        bridge.stop()
        // Every connection ends at once, whatever was in flight.
        #expect(bridge.waitUntilIdle(timeout: .seconds(10)))
        #expect(Date().timeIntervalSince(began) < 5)
        #expect(bridge.peerCount == 0)
        for _ in 0..<3 { #expect(guestEnded.wait(timeout: .now() + 10) == .success) }
        for computer in computers {
            // What was in flight may be read; then the stream ends.
            var buffer = [UInt8](repeating: 0, count: 64 * 1024)
            var got = 1
            while got > 0 { got = Darwin.read(computer.fd, &buffer, buffer.count) }
            #expect(got == 0 || errno == ECONNRESET)
        }
        for _ in 0..<3 { release.signal() }
    }

    @Test func stoppingEndsAConnectionWhoseComputerClosedItsSendingSideAndReadsNothing() throws {
        // On Darwin `shutdown` does nothing to a TCP socket whose peer has
        // closed its end, so a thread blocked writing to it has to be woken
        // some other way.
        let ended = DispatchSemaphore(value: 0)
        let guest = LoopbackGuest { fd in
            let block = patternData(64 * 1024)
            while LoopbackGuest.writeAll(fd, block) {}
            ended.signal()
        }
        let (bridge, port) = try started(guest)
        let computer = try Computer(port: port)
        computer.finishSending()
        Thread.sleep(forTimeInterval: 0.5)
        bridge.stop()
        #expect(bridge.waitUntilIdle(timeout: .seconds(10)))
        #expect(ended.wait(timeout: .now() + 10) == .success)
        #expect(bridge.peerCount == 0)
        _ = computer
    }

    @Test func startingWhatRunsChangesNothingAndStoppingWhatStoppedToo() throws {
        let guest = LoopbackGuest(handler: LoopbackGuest.echo)
        let (bridge, port) = try started(guest)
        #expect(try bridge.start() == port)
        #expect(bridge.isRunning)
        bridge.stop()
        bridge.stop()
        #expect(!bridge.isRunning)
    }

    @Test func aPortInUseIsReportedAsSuch() throws {
        let guest = LoopbackGuest(handler: LoopbackGuest.echo)
        let (first, port) = try started(guest)
        defer { first.stop() }
        let second = AdbNetworkBridge(port: port, opener: guest)
        #expect(throws: AdbError.io(EADDRINUSE)) { try second.start() }
        #expect(!second.isRunning)
    }
}

final class ByteCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var count = 0

    var value: Int { lock.withLock { count } }
    func add(_ amount: Int) { lock.withLock { count += amount } }
    func set(_ amount: Int) { lock.withLock { count = amount } }
}

final class DataBox: @unchecked Sendable {
    private let lock = NSLock()
    private var data: Data?

    var value: Data? { lock.withLock { data } }
    func set(_ data: Data) { lock.withLock { self.data = data } }
}

struct LanAddressTests {
    private func address(_ name: String, _ octets: [UInt8], active: Bool = true, loopback: Bool = false)
        -> InterfaceAddress
    {
        InterfaceAddress(name: name, isActive: active, isLoopback: loopback, octets: octets)
    }

    @Test func wiFiIsTheAddressToShow() {
        let found = LanAddress.choose(from: [
            address("lo0", [127, 0, 0, 1], loopback: true),
            address("pdp_ip0", [10, 77, 3, 9]),
            address("en2", [192, 0, 2, 40]),
            address("en0", [192, 0, 2, 23]),
        ])
        #expect(found == "192.0.2.23")
    }

    @Test func aWiredAdapterServesWhenThereIsNoWiFi() {
        #expect(LanAddress.choose(from: [address("en3", [198, 51, 100, 8])]) == "198.51.100.8")
    }

    @Test func nothingLocalMeansNoAddress() {
        let found = LanAddress.choose(from: [
            address("lo0", [127, 0, 0, 1], loopback: true),
            address("pdp_ip0", [10, 77, 3, 9]),
            address("utun2", [100, 64, 0, 2]),
            address("en0", [169, 254, 12, 1]),
            address("en1", [192, 0, 2, 5], active: false),
            address("en4", [0, 0, 0, 0]),
        ])
        #expect(found == nil)
        #expect(LanAddress.choose(from: []) == nil)
    }

    @Test func theDevicesOwnAddressesAreReadable() {
        // Whatever the host has, it is an address or nothing, never a crash.
        let found = LanAddress.current()
        #expect(found == nil || found!.split(separator: ".").count == 4)
    }
}

struct UsbDebuggingTests {
    @Test func theSettingIsReadFromTheShellOutput() {
        #expect(UsbDebugging.parse("1\n") == true)
        #expect(UsbDebugging.parse("0\n") == false)
        #expect(UsbDebugging.parse("null\n") == false)
        #expect(UsbDebugging.parse("") == nil)
        #expect(UsbDebugging.parse("Error: cmd settings failed") == nil)
    }

    @Test func aFailedCommandSaysNothing() {
        #expect(UsbDebugging.parse(ShellResult(stdout: Data("1\n".utf8), exitCode: 0)) == true)
        #expect(UsbDebugging.parse(ShellResult(stdout: Data("1\n".utf8), exitCode: 1)) == nil)
        #expect(UsbDebugging.parse(ShellResult(stdout: Data("1\n".utf8), exitCode: nil)) == nil)
    }
}
