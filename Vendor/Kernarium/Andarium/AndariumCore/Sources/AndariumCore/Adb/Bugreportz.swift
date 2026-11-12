// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// A line `bugreportz -p` prints (AOSP `frameworks/native/cmds/bugreportz`).
public enum BugreportzLine: Equatable, Sendable {
    case begin(String)
    /// `PROGRESS:n/total`: dumpstate's own estimate of how far it has got.
    case progress(Int, Int)
    /// `OK:<path>`: the report is ready at `path` in the guest.
    case ok(String)
    /// `FAIL:<reason>`
    case fail(String)
    case other(String)

    init(_ line: String) {
        if let rest = line.dropping(prefix: "BEGIN:") {
            self = .begin(rest)
        } else if let rest = line.dropping(prefix: "PROGRESS:") {
            let parts = rest.split(separator: "/", omittingEmptySubsequences: false)
            if parts.count == 2, let done = Int(parts[0]), let total = Int(parts[1]), total > 0, done >= 0 {
                self = .progress(done, total)
            } else {
                self = .other(line)
            }
        } else if let rest = line.dropping(prefix: "OK:") {
            self = .ok(rest)
        } else if let rest = line.dropping(prefix: "FAIL:") {
            self = .fail(rest)
        } else {
            self = .other(line)
        }
    }
}

private extension String {
    func dropping(prefix: String) -> String? {
        hasPrefix(prefix) ? String(dropFirst(prefix.count)) : nil
    }
}

/// Splits `bugreportz -p`'s stdout into lines.
public struct BugreportzParser: Sendable {
    private var buffer = Data()

    public init() {}

    public mutating func feed(_ data: Data) -> [BugreportzLine] {
        buffer.append(data)
        var lines: [BugreportzLine] = []
        while let newline = buffer.firstIndex(of: 0x0A) {
            let text = String(decoding: buffer[buffer.startIndex..<newline], as: UTF8.self)
            buffer = Data(buffer[(newline + 1)...])
            let line = text.trimmingCharacters(in: .whitespacesAndNewlines)
            if !line.isEmpty { lines.append(BugreportzLine(line)) }
        }
        return lines
    }

    /// The last line, which `bugreportz` may leave without a newline.
    public mutating func finish() -> [BugreportzLine] {
        guard !buffer.isEmpty else { return [] }
        return feed(Data([0x0A]))
    }
}

/// Where a bug report's bytes go.
public protocol AdbSink: AnyObject {
    func write(_ data: Data) throws
}

/// The guest's own bug report (`bugreportz -p`, then a sync pull).
public enum BugReport {
    public struct Hooks {
        /// dumpstate's progress, 0 to 1, never moving back.
        public var generating: (Double) -> Void = { _ in }
        /// The report is ready at `remotePath` and is `size` bytes: the
        /// caller checks its room and makes the sink, or throws to stop.
        public var stage: (_ remotePath: String, _ size: UInt64) throws -> any AdbSink = { _, _ in
            throw AdbError.cancelled
        }
        /// Bytes pulled so far, and the total.
        public var pulling: (UInt64, UInt64) -> Void = { _, _ in }
        /// Called whenever the guest shows a sign of life.
        public var alive: () -> Void = {}
        public init() {}
    }

    public struct Result: Equatable, Sendable {
        public var remotePath: String
        public var bytes: UInt64
    }

    /// Runs `bugreportz -p` and returns the path of the report. `alive`
    /// follows each line; a `FAIL` line is `AdbError.remote`.
    public static func generate(
        on connection: AdbConnection, progress: (Double) -> Void, alive: () -> Void
    ) throws -> String {
        let stream = try connection.open(AdbConnection.shellService("bugreportz -p"))
        defer { stream.close() }
        var decoder = ShellV2Decoder()
        var parser = BugreportzParser()
        var fraction = 0.0
        var path: String?
        func handle(_ lines: [BugreportzLine]) throws {
            for line in lines {
                alive()
                switch line {
                case let .progress(done, total):
                    let next = min(Double(done) / Double(total), 1)
                    if next > fraction {
                        fraction = next
                        progress(next)
                    }
                case let .ok(value):
                    path = value
                case let .fail(reason):
                    throw AdbError.remote(reason)
                case .begin, .other:
                    break
                }
            }
        }
        while let chunk = try stream.read() {
            alive()
            for packet in try decoder.feed(chunk) where packet.id == ShellPacket.stdout {
                try handle(parser.feed(packet.data))
            }
            if path != nil { break }
        }
        try handle(parser.finish())
        guard let path else { throw AdbError.guestGone }
        return path
    }

    /// Pulls `remotePath` into the sink `stage` makes.
    public static func pull(
        on connection: AdbConnection, remotePath: String, hooks: Hooks
    ) throws -> Result {
        let sync = try AdbSync(connection: connection)
        defer { sync.quit() }
        let stat = try sync.stat(remotePath)
        guard stat.exists else { throw AdbError.remote("the report is not there") }
        let sink = try hooks.stage(remotePath, stat.size)
        var received: UInt64 = 0
        hooks.pulling(0, stat.size)
        try sync.receive(remotePath) { data in
            try sink.write(data)
            received += UInt64(data.count)
            hooks.alive()
            hooks.pulling(received, stat.size)
        }
        guard received == stat.size else {
            throw AdbError.protocolViolation("the report is \(received) bytes, not \(stat.size)")
        }
        return Result(remotePath: remotePath, bytes: received)
    }

    /// A file name for the staged copy of `remotePath`: its last component,
    /// reduced to characters that are safe in a file name, and ending in .zip.
    public static func stagedName(for remotePath: String) -> String {
        let last = remotePath.split(separator: "/").last.map(String.init) ?? ""
        var name = String(last.unicodeScalars.map { scalar -> Character in
            switch scalar {
            case "a"..."z", "A"..."Z", "0"..."9", ".", "-", "_": Character(scalar)
            default: "_"
            }
        })
        while name.hasPrefix(".") { name.removeFirst() }
        if name.isEmpty { name = "bugreport" }
        if !name.lowercased().hasSuffix(".zip") { name += ".zip" }
        return String(name.prefix(120))
    }

    /// What deletes a report in the guest. `path` is quoted for the shell.
    public static func removalCommand(for path: String) -> String {
        "rm -f \(shellQuoted(path))"
    }

    /// What clears the guest after a pull: the report itself, and the
    /// reports and logs an earlier run left in its directory (every one
    /// older than this report). A path with an unusual character gets the
    /// plain removal.
    public static func cleanupCommand(afterPulling path: String) -> String {
        let safe = path.unicodeScalars.allSatisfy { scalar in
            switch scalar {
            case "a"..."z", "A"..."Z", "0"..."9", ".", "-", "_", "/": true
            default: false
            }
        }
        guard safe, path.hasPrefix("/"), let slash = path.lastIndex(of: "/"), slash != path.startIndex,
              path.index(after: slash) != path.endIndex
        else { return removalCommand(for: path) }
        let directory = String(path[path.startIndex..<slash])
        let name = String(path[path.index(after: slash)...])
        let older = "for f in bugreport-* dumpstate_log-*; do [ \"$f\" -ot '\(name)' ] && rm -f -- \"$f\"; done"
        return "cd '\(directory)' && \(older); rm -f -- '\(name)'"
    }

    /// Stops a report under way. `bugreportz` ends with its stream; the dump
    /// itself is a service of its own.
    public static let stopCommand = "setprop ctl.stop dumpstatez"

    static func shellQuoted(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
}
