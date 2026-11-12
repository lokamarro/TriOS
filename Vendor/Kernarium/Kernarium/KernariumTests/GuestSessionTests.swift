// SPDX-License-Identifier: GPL-2.0-or-later
import Darwin
import Foundation
import Testing

/// Holds the application open for one complete guest session and records what
/// it printed.
///
/// An iOS application on macOS cannot be started from a command line: `open`
/// refuses the bundle and `devicectl` does not enumerate the host Mac, so
/// Xcode's local install service, driven by `xcodebuild test`, is the only
/// launcher. That service stops the application as soon as the tests finish,
/// which is far sooner than a guest needs, and it keeps the application's
/// standard output only inside its own log. This case therefore waits for the
/// session to end and tees the output into the application container, where the
/// host-side lane can read it back. It is inert unless a session is requested,
/// so the ordinary suite neither waits nor redirects anything.
struct GuestSessionTests {
    private static let requestKey = "KERNARIUM_GUEST_SESSION"
    private static let logKey = "KERNARIUM_GUEST_SESSION_LOG"
    private static let timeoutKey = "KERNARIUM_GUEST_SESSION_TIMEOUT"
    private static let defaultLogName = "guest-session.log"
    private static let defaultTimeout = 1800.0
    /// The launcher prints this once the runtime has returned, whether the
    /// guest powered itself off or the runtime refused to start.
    private static let completionMarker = "Kernarium: runtime returned"

    private static var requested: Bool {
        ProcessInfo.processInfo.environment[requestKey] == "1"
    }

    /// Copies everything written to descriptor 1 into `destination` while
    /// leaving the original destination intact, so the Xcode log still shows
    /// the session.
    private final class OutputTee {
        private let original: Int32
        private let readEnd: Int32
        private let sink: Int32
        private let worker: Thread
        private let copied: DispatchSemaphore

        init(destination: URL) throws {
            FileManager.default.createFile(atPath: destination.path, contents: nil)
            sink = open(destination.path, O_WRONLY | O_TRUNC)
            guard sink >= 0 else {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
            original = dup(STDOUT_FILENO)
            var descriptors: [Int32] = [-1, -1]
            guard original >= 0, pipe(&descriptors) == 0 else {
                close(sink)
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
            readEnd = descriptors[0]
            dup2(descriptors[1], STDOUT_FILENO)
            close(descriptors[1])
            // A pipe makes stdio fully buffered, which would hold the session's
            // records back until the application exits.
            setvbuf(stdout, nil, _IOLBF, 0)
            let source = readEnd
            let copies = [original, sink]
            let done = DispatchSemaphore(value: 0)
            copied = done
            worker = Thread {
                defer { done.signal() }
                var buffer = [UInt8](repeating: 0, count: 16384)
                while true {
                    let received = buffer.withUnsafeMutableBytes {
                        read(source, $0.baseAddress, $0.count)
                    }
                    if received < 0 && errno == EINTR { continue }
                    if received <= 0 { break }
                    for descriptor in copies {
                        var offset = 0
                        while offset < received {
                            let written = buffer.withUnsafeBytes {
                                write(
                                    descriptor,
                                    $0.baseAddress!.advanced(by: offset),
                                    received - offset
                                )
                            }
                            if written <= 0 { break }
                            offset += written
                        }
                    }
                }
            }
            worker.name = "uml-session-tee"
            worker.start()
        }

        /// Restores descriptor 1 and returns once every record written
        /// before it has reached the log.
        func finish() {
            fflush(stdout)
            // Descriptor 1 held the pipe's last write end, so the copier
            // reads the end of the pipe once it has copied what came before.
            dup2(original, STDOUT_FILENO)
            if copied.wait(timeout: .now() + 30) == .timedOut {
                // Something else still holds a write end. The copier may
                // still be using these descriptors, so they stay open rather
                // than be reused under it.
                print("Kernarium: session log tee did not drain")
                return
            }
            close(readEnd)
            close(sink)
            close(original)
        }
    }

    private static func documentsDirectory() throws -> URL {
        try #require(
            FileManager.default.urls(
                for: .documentDirectory, in: .userDomainMask
            ).first,
            "the application container has no Documents directory"
        )
    }

    @Test(.enabled(if: GuestSessionTests.requested))
    func holdsTheApplicationOpenForOneGuestSession() throws {
        let environment = ProcessInfo.processInfo.environment
        let log = try Self.documentsDirectory().appendingPathComponent(
            environment[Self.logKey] ?? Self.defaultLogName
        )
        let deadline = Date().addingTimeInterval(
            environment[Self.timeoutKey].flatMap(Double.init) ?? Self.defaultTimeout
        )
        let tee = try OutputTee(destination: log)
        defer { tee.finish() }

        print("KERNARIUM_SESSION_BEGIN|log=\(log.lastPathComponent)")
        var completed = false
        while Date() < deadline {
            Thread.sleep(forTimeInterval: 2)
            guard let recorded = try? String(contentsOf: log, encoding: .utf8)
            else { continue }
            if recorded.contains(Self.completionMarker) {
                completed = true
                break
            }
        }
        print("KERNARIUM_SESSION_END|completed=\(completed)")
        #expect(completed, "the guest session did not finish before its deadline")
    }
}
