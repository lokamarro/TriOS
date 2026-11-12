// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// Opens a stream to the guest's adbd for a computer that reached the
/// listener. The call blocks; the descriptor it returns is a connected
/// `SOCK_STREAM` socket the caller owns.
public protocol GuestStreamOpener: Sendable {
    func openStream() throws -> Int32
}

/// ADB over the network: a listener on the device's network addresses whose
/// every accepted connection is joined to a fresh stream to the guest's adbd.
/// The bridge copies bytes both ways and understands none of them, so the
/// guest's adbd does the authentication, and a computer whose key the guest
/// does not know gets the guest's own prompt.
///
/// Every connection has two threads, one for each direction. A thread waits
/// until its source can be read, reads one block, and writes it, waiting
/// until the sink can take the rest. A slow side therefore holds back its
/// sender through the socket buffers (TCP's window; the vsock device's
/// credit), and nothing is buffered beyond one 64 KiB block per direction.
/// Every wait also watches a pipe of the connection's own, which is how a
/// stop or a failure ends both threads at once: on Darwin, `shutdown` does
/// nothing to a TCP socket whose peer has already closed its end, so it
/// cannot be what wakes a thread blocked in a write. When one side ends, the
/// end passes on as a half close, and the connection goes when both
/// directions are done. A computer that vanishes is found by TCP keepalive.
public final class AdbNetworkBridge: @unchecked Sendable {
    public static let defaultPort: UInt16 = 5555
    /// The most computers connected at once. The runtime allows 32 open
    /// streams to the guest in all; the rest are for the app's own use.
    public static let defaultMaxPeers = 8

    private let port: UInt16
    private let maxPeers: Int
    private let opener: any GuestStreamOpener

    private let lock = NSLock()
    private var running = false
    private var boundPort: UInt16 = 0
    private var wakeWriter: Int32 = -1
    private var links: [ObjectIdentifier: Link] = [:]
    private var acceptFinished: DispatchSemaphore?
    /// Threads alive, for `waitUntilIdle`.
    private let threadsIdle = NSCondition()
    private var threads = 0

    /// `port` 0 asks for any free port, which tests use.
    public init(
        port: UInt16 = AdbNetworkBridge.defaultPort, maxPeers: Int = AdbNetworkBridge.defaultMaxPeers,
        opener: any GuestStreamOpener
    ) {
        self.port = port
        self.maxPeers = maxPeers
        self.opener = opener
    }

    /// The number of computers connected now.
    public var peerCount: Int { lock.withLock { links.count } }

    public var isRunning: Bool { lock.withLock { running } }

    /// Binds the listener on every IPv4 address and starts accepting. Returns
    /// the port bound. Throws `AdbError.io(EADDRINUSE)` when another
    /// listener holds the port. Starting a running bridge changes nothing.
    @discardableResult
    public func start() throws -> UInt16 {
        try lock.withLock { () throws -> UInt16 in
            if running { return boundPort }
            let listener = try Self.makeListener(port: port)
            var pipeEnds: [Int32] = [-1, -1]
            guard pipe(&pipeEnds) == 0 else {
                let code = errno
                Darwin.close(listener)
                throw AdbError.io(code)
            }
            let wakeReader = pipeEnds[0]
            let bound: UInt16
            do {
                bound = try Self.localPort(of: listener)
            } catch {
                Darwin.close(listener)
                Darwin.close(wakeReader)
                Darwin.close(pipeEnds[1])
                throw error
            }
            running = true
            boundPort = bound
            wakeWriter = pipeEnds[1]
            let finished = DispatchSemaphore(value: 0)
            acceptFinished = finished
            spawn(name: "andarium-adb-listener") { [self] in
                acceptLoop(listener: listener, wake: wakeReader)
                Darwin.close(listener)
                Darwin.close(wakeReader)
                finished.signal()
            }
            return bound
        }
    }

    /// Stops accepting, ends every connection, and returns once the port is
    /// free again. Stopping a stopped bridge changes nothing.
    public func stop() {
        let taken: (links: [Link], writer: Int32, finished: DispatchSemaphore?)? = lock.withLock {
            guard running else { return nil }
            running = false
            let writer = wakeWriter
            wakeWriter = -1
            let finished = acceptFinished
            acceptFinished = nil
            return (Array(links.values), writer, finished)
        }
        guard let taken else { return }
        var byte: UInt8 = 1
        _ = Darwin.write(taken.writer, &byte, 1)
        Darwin.close(taken.writer)
        for link in taken.links { link.abort() }
        _ = taken.finished?.wait(timeout: .now() + 2)
    }

    /// Waits until every thread of the bridge has ended; false when `timeout`
    /// passed first. For tests, and for a caller that must be sure nothing
    /// still touches a descriptor.
    public func waitUntilIdle(timeout: Duration) -> Bool {
        let deadline = Date().addingTimeInterval(
            Double(timeout.components.seconds) + Double(timeout.components.attoseconds) / 1e18)
        threadsIdle.lock()
        defer { threadsIdle.unlock() }
        while threads > 0 {
            if !threadsIdle.wait(until: deadline) { return threads == 0 }
        }
        return true
    }

    // MARK: Listener

    private static func makeListener(port: UInt16) throws -> Int32 {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw AdbError.io(errno) }
        var on: Int32 = 1
        _ = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, socklen_t(MemoryLayout<Int32>.size))
        _ = setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian
        address.sin_addr = in_addr(s_addr: INADDR_ANY.bigEndian)
        let bound = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bound == 0, listen(fd, 8) == 0 else {
            let code = errno
            Darwin.close(fd)
            throw AdbError.io(code)
        }
        // Polled before every accept; a connection reset in between must not
        // block the loop.
        _ = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)
        return fd
    }

    private static func localPort(of fd: Int32) throws -> UInt16 {
        var address = sockaddr_in()
        var length = socklen_t(MemoryLayout<sockaddr_in>.size)
        let result = withUnsafeMutablePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &length) }
        }
        guard result == 0 else { throw AdbError.io(errno) }
        return UInt16(bigEndian: address.sin_port)
    }

    private func acceptLoop(listener: Int32, wake: Int32) {
        var fds = [
            pollfd(fd: listener, events: Int16(POLLIN), revents: 0),
            pollfd(fd: wake, events: Int16(POLLIN), revents: 0),
        ]
        while true {
            let ready = poll(&fds, 2, -1)
            if ready < 0 {
                if errno == EINTR { continue }
                return
            }
            if fds[1].revents != 0 { return }
            guard fds[0].revents & Int16(POLLIN) != 0 else {
                // An error on the listener itself ends the loop; nothing else
                // would clear it.
                if fds[0].revents != 0 { return }
                continue
            }
            let peer = accept(listener, nil, nil)
            if peer < 0 {
                switch errno {
                case EINTR, EAGAIN, ECONNABORTED, EPROTO:
                    continue
                case EMFILE, ENFILE, ENOBUFS, ENOMEM:
                    // Out of descriptors: wait for some to be freed.
                    usleep(50_000)
                    continue
                default:
                    return
                }
            }
            admit(peer)
        }
    }

    // MARK: Connections

    private func admit(_ peer: Int32) {
        Self.configure(peer: peer)
        guard let link = Link(peer: peer) else {
            Darwin.close(peer)
            return
        }
        let admitted = lock.withLock { () -> Bool in
            guard running, links.count < maxPeers else { return false }
            links[ObjectIdentifier(link)] = link
            return true
        }
        guard admitted else {
            link.closeDescriptors()
            return
        }
        spawn(name: "andarium-adb-peer") { [self] in serve(link) }
    }

    private static func configure(peer: Int32) {
        Link.makeNonBlocking(peer)
        var on: Int32 = 1
        let size = socklen_t(MemoryLayout<Int32>.size)
        _ = setsockopt(peer, SOL_SOCKET, SO_NOSIGPIPE, &on, size)
        _ = setsockopt(peer, IPPROTO_TCP, TCP_NODELAY, &on, size)
        // A computer that went away without a word is found in about a minute.
        _ = setsockopt(peer, SOL_SOCKET, SO_KEEPALIVE, &on, size)
        var idle: Int32 = 30
        var interval: Int32 = 10
        var count: Int32 = 3
        _ = setsockopt(peer, IPPROTO_TCP, TCP_KEEPALIVE, &idle, size)
        _ = setsockopt(peer, IPPROTO_TCP, TCP_KEEPINTVL, &interval, size)
        _ = setsockopt(peer, IPPROTO_TCP, TCP_KEEPCNT, &count, size)
    }

    /// One computer's connection, on its own thread: open the guest's stream,
    /// then carry bytes from the computer to the guest here and from the
    /// guest to the computer on another.
    private func serve(_ link: Link) {
        let guest: Int32
        do {
            guest = try opener.openStream()
        } catch {
            finish(link, closing: nil)
            return
        }
        var on: Int32 = 1
        _ = setsockopt(guest, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        Link.makeNonBlocking(guest)
        guard link.attach(guest: guest) else {
            // Stopped while the stream was opening.
            finish(link, closing: guest)
            return
        }
        spawn(name: "andarium-adb-guest") { [self] in
            pump(from: guest, to: link.peer, link: link)
            if link.pumpEnded() { finish(link, closing: nil) }
        }
        pump(from: link.peer, to: guest, link: link)
        if link.pumpEnded() { finish(link, closing: nil) }
    }

    private func pump(from source: Int32, to sink: Int32, link: Link) {
        var buffer = [UInt8](repeating: 0, count: 64 * 1024)
        while true {
            guard link.wait(for: source, events: Int16(POLLIN)) else { return }
            let got = buffer.withUnsafeMutableBytes { Darwin.read(source, $0.baseAddress, $0.count) }
            if got == 0 {
                // The sender is done; the receiver hears it as the end of
                // the stream, and its own reply still flows the other way.
                _ = Darwin.shutdown(sink, SHUT_WR)
                return
            }
            if got < 0 {
                if errno == EINTR || errno == EAGAIN { continue }
                link.abort()
                return
            }
            var done = 0
            while done < got {
                let wrote = buffer.withUnsafeBytes { Darwin.write(sink, $0.baseAddress! + done, got - done) }
                if wrote > 0 {
                    done += wrote
                } else if wrote < 0 && errno == EINTR {
                    continue
                } else if wrote < 0 && errno == EAGAIN {
                    // The sink is full: the source is not read again until it
                    // takes the rest, which is the backpressure.
                    guard link.wait(for: sink, events: Int16(POLLOUT)) else { return }
                } else {
                    link.abort()
                    return
                }
            }
        }
    }

    /// Closes the connection's descriptors, once nothing reads or writes
    /// them, and forgets it.
    private func finish(_ link: Link, closing extra: Int32?) {
        if let extra { Darwin.close(extra) }
        link.closeDescriptors()
        lock.withLock { _ = links.removeValue(forKey: ObjectIdentifier(link)) }
    }

    private func spawn(name: String, _ body: @escaping @Sendable () -> Void) {
        threadsIdle.lock()
        threads += 1
        threadsIdle.unlock()
        let thread = Thread { [self] in
            body()
            threadsIdle.lock()
            threads -= 1
            threadsIdle.broadcast()
            threadsIdle.unlock()
        }
        thread.name = name
        thread.qualityOfService = .userInitiated
        thread.start()
    }

    /// The two descriptors of one connection, the pipe that wakes its threads,
    /// and who may still use them.
    private final class Link: @unchecked Sendable {
        let peer: Int32
        private let wakeReader: Int32
        private let wakeWriter: Int32
        private let lock = NSLock()
        private var guest: Int32 = -1
        private var aborted = false
        private var closed = false
        private var pumpsRunning = 0

        init?(peer: Int32) {
            var ends: [Int32] = [-1, -1]
            guard pipe(&ends) == 0 else { return nil }
            self.peer = peer
            wakeReader = ends[0]
            wakeWriter = ends[1]
            Self.makeNonBlocking(ends[0])
            Self.makeNonBlocking(ends[1])
        }

        static func makeNonBlocking(_ descriptor: Int32) {
            _ = fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) | O_NONBLOCK)
        }

        /// Gives the connection its guest stream; false when it was aborted
        /// first, in which case the caller closes the stream.
        func attach(guest: Int32) -> Bool {
            lock.withLock {
                guard !aborted else { return false }
                self.guest = guest
                pumpsRunning = 2
                return true
            }
        }

        /// Waits until `descriptor` has one of `events` (or has ended, which
        /// the next read or write reports); false when the connection was
        /// aborted meanwhile.
        func wait(for descriptor: Int32, events: Int16) -> Bool {
            var fds = [
                pollfd(fd: wakeReader, events: Int16(POLLIN), revents: 0),
                pollfd(fd: descriptor, events: events, revents: 0),
            ]
            while true {
                if poll(&fds, 2, -1) < 0 {
                    if errno == EINTR { continue }
                    return false
                }
                if fds[0].revents != 0 { return false }
                if fds[1].revents & Int16(POLLNVAL) != 0 { return false }
                if fds[1].revents != 0 { return true }
            }
        }

        /// Ends both directions at once: a thread waiting for either
        /// descriptor wakes and leaves.
        func abort() {
            lock.withLock {
                aborted = true
                guard !closed else { return }
                var byte: UInt8 = 1
                _ = Darwin.write(wakeWriter, &byte, 1)
                // Tells the far ends sooner, where the system allows it.
                _ = Darwin.shutdown(peer, SHUT_RDWR)
                if guest >= 0 { _ = Darwin.shutdown(guest, SHUT_RDWR) }
            }
        }

        /// A direction is done; true for the last one.
        func pumpEnded() -> Bool {
            lock.withLock {
                pumpsRunning -= 1
                return pumpsRunning == 0
            }
        }

        func closeDescriptors() {
            lock.withLock {
                guard !closed else { return }
                closed = true
                Darwin.close(peer)
                if guest >= 0 { Darwin.close(guest) }
                Darwin.close(wakeReader)
                Darwin.close(wakeWriter)
            }
        }
    }
}
