// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// How the previous process's guest session ended, as the runtime recorded
/// it (darwin/runtime/session_record.hpp).
///
/// The runtime writes "running" when the guest boots and overwrites it with
/// the end on every way the session ends that runs any code: Android's own
/// power-off or restart, a stop, a guest kernel panic with its message, an
/// exit the kernel drives, a hibernated guest whose RAM could not be
/// restored, a crash of a host thread and an exit() of the process. A record
/// still running at the next launch says that the process ended without
/// running any of that code: the system ended it, as an end at the memory
/// limit does, or it crashed outside the guest's paths, which leaves only
/// the system's crash report.
public struct SessionRecord: Equatable, Sendable {
    /// The last of the runtime's memory checkpoints, a few seconds apart.
    public struct Memory: Equatable, Sendable {
        public var at: Date
        public var footprintBytes: UInt64
        public var peakBytes: UInt64
        /// What the process's memory limit still allowed; nil where the host
        /// sets no limit.
        public var availableBytes: UInt64?
    }

    public var running: Bool
    /// The runtime's word for the end: running, powered-off, restarted,
    /// stopped, failed, guest-panic, kernel-abort, kernel-exit,
    /// hibernation-restore-failed, process-exit or host-fault.
    public var end: String
    public var status: Int
    public var processID: Int
    public var started: Date?
    public var ended: Date?
    /// The panic's message, or what else the end said.
    public var message: String?
    /// The guest kernel's last output before a crash, oldest line first.
    public var kernelOutput: String?
    public var memory: Memory?
    /// The footprint's large terms at the last checkpoint and at the
    /// session's highest one, in MiB, as the runtime words them ("guest G gpu
    /// H/B ..."); nil in a record that has none.
    public var terms: String?
    public var peakTerms: String?

    /// Reads a record's text; nil when it is not one.
    public init?(text: String) {
        let parts = text.components(separatedBy: "\nkernel-output\n")
        var fields: [String: String] = [:]
        let lines = parts[0].split(separator: "\n", omittingEmptySubsequences: true)
            .filter { !$0.allSatisfy { $0 == " " } }
        guard let format = lines.first, ["uml-session 1", "uml-session 2", "uml-session 3"].contains(format)
        else { return nil }
        for line in lines.dropFirst() {
            guard let space = line.firstIndex(of: " ") else { continue }
            fields[String(line[..<space])] = String(line[line.index(after: space)...])
        }
        guard let state = fields["state"], let end = fields["end"] else { return nil }
        running = state == "running"
        self.end = end
        status = fields["status"].flatMap { Int($0) } ?? 0
        processID = fields["pid"].flatMap { Int($0) } ?? 0
        started = fields["started"].flatMap { TimeInterval($0) }.map { Date(timeIntervalSince1970: $0) }
        ended = fields["ended"].flatMap { TimeInterval($0) }.map { Date(timeIntervalSince1970: $0) }
        message = fields["message"]
        kernelOutput = parts.count > 1 ? parts[1] : nil
        memory = fields["memory"].flatMap(Self.memory(from:))
        terms = fields["terms"]
        peakTerms = fields["peak-terms"]
    }

    /// "at T footprint F peak P [available A]".
    private static func memory(from line: String) -> Memory? {
        let words = line.split(separator: " ")
        var values: [String: UInt64] = [:]
        var index = 0
        while index + 1 < words.count {
            if let value = UInt64(words[index + 1]) { values[String(words[index])] = value }
            index += 2
        }
        guard let at = values["at"], let footprint = values["footprint"] else { return nil }
        return Memory(
            at: Date(timeIntervalSince1970: TimeInterval(at)), footprintBytes: footprint,
            peakBytes: values["peak"] ?? footprint, availableBytes: values["available"]
        )
    }

    /// The record at `url`, or nil when there is none or it is not a record.
    public static func read(at url: URL) -> SessionRecord? {
        guard let data = try? Data(contentsOf: url), let text = String(data: data, encoding: .utf8) else {
            return nil
        }
        return SessionRecord(text: text)
    }

    /// Whether the session ended on its own terms: Android's power-off or
    /// restart, or a stop.
    public var orderly: Bool {
        !running && ["powered-off", "restarted", "stopped"].contains(end)
    }

    /// Below this, at the last checkpoint, a record still running most
    /// likely ended at the memory limit: the GPU and the guest can take that
    /// much between two checkpoints.
    public static let lowMemoryBytes: UInt64 = 256 << 20

    /// Whether the last checkpoint left the process close to its limit.
    public var nearMemoryLimit: Bool {
        guard let available = memory?.availableBytes else { return false }
        return available < Self.lowMemoryBytes
    }

    /// One line for a log or a report.
    public var summary: String {
        if running {
            var line = "The previous session was still running when the process ended (pid \(processID))"
            if let memory {
                line += "; at the last checkpoint it used \(memory.footprintBytes >> 20) MiB"
                if let available = memory.availableBytes { line += ", \(available >> 20) MiB left" }
            }
            return line + "."
        }
        let detail = message.map { ": \($0)" } ?? ""
        switch end {
        case "guest-panic":
            return "The previous session ended in a guest kernel panic\(detail)."
        case "kernel-abort":
            return "The previous session ended in a guest kernel crash\(detail)."
        case "kernel-exit":
            return "The guest kernel ended the previous process with status \(status)\(detail)."
        case "hibernation-restore-failed":
            return "The previous session ended because its guest RAM could not be restored\(detail)."
        case "process-exit":
            return "The previous process exited while its guest ran\(detail)."
        case "host-fault":
            return "The previous process crashed in Andarium's own code, with a crash report\(detail)."
        default:
            return "The previous session ended: \(end)\(detail)."
        }
    }

    /// What Home says at the next launch, or nil for an orderly end. A record
    /// still running whose process ended out of the foreground is iOS
    /// closing a paused guest, which the pause marker reports instead.
    public func notice(endedOutOfForeground: Bool) -> SessionEndNotice? {
        if orderly || (running && endedOutOfForeground) { return nil }
        switch end {
        case "guest-panic":
            return SessionEndNotice(
                title: "Android's kernel panicked",
                text: "Android stopped with a kernel panic" + (message.map { ": \($0)." } ?? ".")
            )
        case "kernel-abort":
            return SessionEndNotice(title: "Android's kernel crashed", text: "Android stopped on a kernel error.")
        case "kernel-exit":
            return SessionEndNotice(title: "Android stopped", text: "Android's kernel ended Andarium.")
        case "hibernation-restore-failed":
            return SessionEndNotice(
                title: "Android could not resume",
                text: "Android's memory could not be restored, so it starts again from boot."
            )
        case "host-fault", "process-exit":
            return SessionEndNotice(
                title: "Andarium crashed", text: "Andarium stopped on an error of its own while Android was running."
            )
        case "running" where nearMemoryLimit:
            return SessionEndNotice(
                title: "iOS ended Andarium",
                text: "Android was using almost all the memory iOS allows Andarium. A smaller memory size is safer."
            )
        case "running":
            return SessionEndNotice(
                title: "Andarium stopped unexpectedly", text: "Andarium ended while Android was running."
            )
        default:
            return SessionEndNotice(title: "Android stopped", text: "Android stopped unexpectedly.")
        }
    }
}

/// Home's card after a session that ended badly.
public struct SessionEndNotice: Equatable, Sendable {
    public var title: String
    public var text: String

    public init(title: String, text: String) {
        self.title = title
        self.text = text
    }
}

/// Text that leaves the device in a bug report, with the application's own
/// container paths taken out: they identify this install of Andarium.
public enum DiagnosticRedaction {
    /// `text` with every path in `containers` replaced by `<container>`, and
    /// any other path that names an application container by its identifier
    /// replaced the same way.
    public static func redact(_ text: String, containers: [String]) -> String {
        var result = text
        for container in containers.filter({ $0.count > 1 }).sorted(by: { $0.count > $1.count }) {
            result = result.replacingOccurrences(of: container, with: "<container>")
            // /private/var and /var name the same directory.
            if container.hasPrefix("/private/var/") {
                result = result.replacingOccurrences(
                    of: String(container.dropFirst("/private".count)), with: "<container>")
            }
        }
        let identified = #"(/private)?/var/[^\s"']*?[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}"#
        result = result.replacingOccurrences(of: identified, with: "<container>", options: .regularExpression)
        // An iPad app on a Mac lives under the person's home directory.
        return result.replacingOccurrences(
            of: #"/Users/[^/\s"']+"#, with: "/Users/<user>", options: .regularExpression)
    }
}

/// The previous session's record at launch: read once, then kept as
/// `previous-session.txt` for the bug report, so the next boot can write its
/// own and a second launch says nothing more.
public enum PreviousSession {
    /// The record and the notice Home shows for it. `endedOutOfForeground`
    /// is whether the pause marker said the previous process ended paused.
    public static func consume(in diagnostics: URL, endedOutOfForeground: Bool)
        -> (record: SessionRecord, notice: HomeNotice?)?
    {
        let current = diagnostics.appendingPathComponent(BugReportAttachments.sessionRecord)
        guard let record = SessionRecord.read(at: current) else { return nil }
        let kept = diagnostics.appendingPathComponent(BugReportAttachments.previousSessionRecord)
        try? FileManager.default.removeItem(at: kept)
        try? FileManager.default.moveItem(at: current, to: kept)
        return (record, record.notice(endedOutOfForeground: endedOutOfForeground).map { .sessionEnded($0) })
    }
}
