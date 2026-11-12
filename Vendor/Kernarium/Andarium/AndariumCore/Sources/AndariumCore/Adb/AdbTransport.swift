// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// A reliable, ordered byte stream to adbd. The application gives the client a
/// descriptor the runtime opened to the guest's vsock port; the tests give it
/// a scripted adbd. Reads and writes block, on a thread of the operation's own.
public protocol AdbTransport: AnyObject, Sendable {
    /// Blocks until `count` bytes have arrived. Throws `AdbError.guestGone`
    /// when the stream ends first.
    func readExactly(_ count: Int) throws -> Data
    /// Blocks until every byte is written.
    func writeAll(_ data: Data) throws
    /// Ends the connection from any thread, which unblocks a read or write in
    /// progress on another. Calling it again changes nothing.
    func shutdown()
}

/// Opens connections to adbd. A call blocks; it throws `AdbError.refused`
/// when nothing answers yet, which the caller retries.
public protocol AdbConnector: Sendable {
    func connect() throws -> any AdbTransport
}

/// A connected stream socket as a transport. It owns the descriptor and
/// closes it when released.
public final class DescriptorTransport: AdbTransport, @unchecked Sendable {
    private let lock = NSLock()
    private var descriptor: Int32
    private var ended = false

    /// Takes ownership of `descriptor`, a connected `SOCK_STREAM` socket.
    public init(descriptor: Int32) {
        self.descriptor = descriptor
        // A write to a socket the peer or `shutdown` ended must fail with
        // EPIPE and not raise SIGPIPE.
        var on: Int32 = 1
        _ = setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
    }

    deinit {
        if descriptor >= 0 { Darwin.close(descriptor) }
    }

    public func readExactly(_ count: Int) throws -> Data {
        var data = Data(count: count)
        var done = 0
        while done < count {
            let fd = try currentDescriptor()
            try waitReadable(fd)
            let got = data.withUnsafeMutableBytes { buffer in
                Darwin.read(fd, buffer.baseAddress! + done, count - done)
            }
            if got > 0 {
                done += got
            } else if got == 0 {
                throw AdbError.guestGone
            } else if errno == EINTR || errno == EAGAIN {
                continue
            } else {
                throw Self.translated(errno)
            }
        }
        return data
    }

    public func writeAll(_ data: Data) throws {
        var done = 0
        while done < data.count {
            let fd = try currentDescriptor()
            let wrote = data.withUnsafeBytes { buffer in
                Darwin.write(fd, buffer.baseAddress! + done, data.count - done)
            }
            if wrote >= 0 {
                done += wrote
            } else if errno == EINTR {
                continue
            } else {
                throw Self.translated(errno)
            }
        }
    }

    public func shutdown() {
        lock.withLock {
            guard !ended, descriptor >= 0 else { return }
            ended = true
            _ = Darwin.shutdown(descriptor, SHUT_RDWR)
        }
    }

    private func currentDescriptor() throws -> Int32 {
        try lock.withLock {
            guard !ended, descriptor >= 0 else { throw AdbError.guestGone }
            return descriptor
        }
    }

    /// Waits until `fd` can be read or has ended. A `read` that blocks on a
    /// Darwin socket pair can miss the end its peer announces with
    /// `shutdown(SHUT_WR)` when the two happen together, and then sleeps for
    /// ever; `poll` does not miss it, and a local `shutdown` wakes it too.
    /// `poll` never wakes for a descriptor closed under it, though, so the
    /// wait also looks for a `shutdown` once a second: a stray close
    /// elsewhere in the process cannot hold it past a cancel or a timeout.
    private func waitReadable(_ fd: Int32) throws {
        var entry = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
        while true {
            let ready = poll(&entry, 1, 1000)
            if ready > 0 { return }
            if ready < 0, errno != EINTR { throw Self.translated(errno) }
            _ = try currentDescriptor()
        }
    }

    private static func translated(_ code: Int32) -> AdbError {
        switch code {
        case ECONNRESET, EPIPE, ENOTCONN, ECONNABORTED, EBADF: .guestGone
        default: .io(code)
        }
    }
}
