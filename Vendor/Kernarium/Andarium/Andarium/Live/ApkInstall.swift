// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// Lets a cancel reach a coordinated read that is waiting for iCloud.
nonisolated final class FileCoordinatorBox: @unchecked Sendable {
    private let lock = NSLock()
    private let coordinator = NSFileCoordinator()

    var current: NSFileCoordinator { coordinator }

    func cancel() {
        lock.withLock { coordinator.cancel() }
    }
}

/// Installs one APK through the package manager's `abb_exec` service: the
/// file goes over in 256 KiB reads, never whole in memory, and the output the
/// stream ends with is `Success` or `Failure [CODE: text]`.
nonisolated enum ApkInstallOperation {
    static let chunkBytes = 256 * 1024
    /// Sending the file is eight tenths of the bar; Android's own install,
    /// whose length is not known, holds there.
    static let sendingShare = 0.8
    static let sendIdleLimit = Duration.seconds(60)
    static let commitLimit = Duration.seconds(15 * 60)

    /// `-r` replaces an installed app, as the person expects of an APK for
    /// one already there; `-t` admits test-only builds; `-S` gives the size.
    static func service(size: Int) -> String {
        "abb_exec:package\0install\0-r\0-t\0-S\0\(size)"
    }

    static func run(
        session: AdbSession, coordinator: FileCoordinatorBox, url: URL, size: Int, fileInfo: String,
        update: @escaping @Sendable (BusyUpdate) -> Void
    ) throws {
        guard size > 0 else { throw AdbFailure(message: PackageInstallMessages.message(forCode: "FAILED_INVALID_APK")) }
        var result: Result<Void, Error> = .success(())
        var coordinationError: NSError?
        // The read is coordinated, which brings an iCloud file down first.
        update(BusyUpdate(detail: "Getting the file\u{2026}"))
        coordinator.current.coordinate(readingItemAt: url, options: [], error: &coordinationError) { readable in
            result = Result { try send(session: session, file: readable, size: size, fileInfo: fileInfo, update: update) }
        }
        if let coordinationError { throw coordinationError }
        try result.get()
    }

    private static func send(
        session: AdbSession, file: URL, size: Int, fileInfo: String,
        update: @escaping @Sendable (BusyUpdate) -> Void
    ) throws {
        let handle = try FileHandle(forReadingFrom: file)
        defer { try? handle.close() }
        let watchdog = session.watchdog
        try session.run { connection in
            update(BusyUpdate(detail: fileInfo))
            watchdog.arm(idle: sendIdleLimit)
            let stream = try connection.open(service(size: size))
            var sent = 0
            do {
                while sent < size {
                    guard let data = try handle.read(upToCount: min(chunkBytes, size - sent)), !data.isEmpty else {
                        throw AdbError.io(EIO)
                    }
                    try stream.write(data)
                    sent += data.count
                    watchdog.touch()
                    update(BusyUpdate(progress: Double(sent) / Double(size) * sendingShare))
                }
            } catch AdbError.streamClosed {
                // The package manager gave up early; what it printed says why.
            }
            update(BusyUpdate(
                progress: sendingShare, detail: "The guest is installing the app\u{2026}", cancellable: false))
            watchdog.arm(idle: nil, overall: commitLimit)
            let output = try stream.readToEnd(limit: 64 * 1024)
            if let message = PackageInstallMessages.failure(inOutput: String(decoding: output, as: UTF8.self)) {
                throw AdbFailure(message: message)
            }
        }
    }
}
