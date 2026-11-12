// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// A guest process that holds graphics memory, as the runtime reports it.
public struct GraphicsHolder: Equatable, Sendable {
    /// The guest process; 0 for a context whose creator the host cannot tell.
    public var pid: UInt32
    public var contexts: UInt32
    /// What the process's graphics contexts hold of the host GPU's memory.
    public var bytes: UInt64
    /// How long its most recently active context has run no command.
    public var idle: Duration
    /// The process's command name, or an unnamed context's debug name.
    public var name: String

    public init(pid: UInt32, contexts: UInt32 = 1, bytes: UInt64, idle: Duration, name: String) {
        self.pid = pid
        self.contexts = contexts
        self.bytes = bytes
        self.idle = idle
        self.name = name
    }
}

/// The guest's graphics memory budget and who holds it, at one instant.
public struct GraphicsMemoryReading: Equatable, Sendable {
    /// The device memory budget; 0 while the host has none.
    public var budgetBytes: UInt64
    /// What the host process holds against it, memory the drivers keep for
    /// reuse included.
    public var usageBytes: UInt64
    /// What the guest's contexts hold of that, named or not.
    public var contextBytes: UInt64
    /// The refusals the guest was answered with, since its display was made.
    public var refusals: UInt64
    /// Whether the guest's own low-memory killer asks the host for the budget's
    /// holders and ends them itself.
    public var guestReclaims: Bool
    /// The holders that are quiet and large enough to be asked for, largest
    /// first.
    public var holders: [GraphicsHolder]

    public init(
        budgetBytes: UInt64, usageBytes: UInt64, contextBytes: UInt64, refusals: UInt64 = 0,
        guestReclaims: Bool = false, holders: [GraphicsHolder] = []
    ) {
        self.budgetBytes = budgetBytes
        self.usageBytes = usageBytes
        self.contextBytes = contextBytes
        self.refusals = refusals
        self.guestReclaims = guestReclaims
        self.holders = holders
    }
}

/// One process of the activity manager's list (`dumpsys activity lru`).
public struct AndroidProcess: Equatable, Sendable {
    /// How far the process may be ended.
    public enum Standing: Equatable, Sendable {
        /// Cached: the system could end it itself, and `cmd activity kill`
        /// ends only these.
        case cached
        /// The previous app, which the person may return to next.
        case previous
        /// Anything else: foreground, visible, perceptible, a service, home.
        case kept
    }

    public var pid: UInt32
    /// The process name, which for an app's main process is its package.
    public var name: String
    /// "u0a118" for an app of user 0, or a system user's number.
    public var uid: String
    /// The activity manager's importance label: "fg", "vis", "prev", "cch".
    public var label: String
    /// Its process state: "TOP", "LAST", "CAC", "CEM".
    public var state: String

    /// Decided by the importance label alone: "cch" is adj 900 and above,
    /// whatever its state ("CEM", "CAC", and "LAST" for the cached activity
    /// of an app that was in front a moment ago).
    public var standing: Standing {
        if label.hasPrefix("cch") { return .cached }
        if label == "prev" { return .previous }
        return .kept
    }

    /// The app's package: its process name without a `:suffix`.
    public var package: String {
        name.split(separator: ":", maxSplits: 1, omittingEmptySubsequences: false).first.map(String.init) ?? name
    }

    /// The Android user of an app's uid ("u0a118" is user 0); nil for a
    /// system uid.
    public var user: Int? {
        guard uid.hasPrefix("u"), let marker = uid.firstIndex(of: "a"), uid.index(after: uid.startIndex) < marker else {
            return nil
        }
        let number = uid[uid.index(after: uid.startIndex)..<marker]
        guard number.allSatisfy(\.isNumber), uid[uid.index(after: marker)...].allSatisfy(\.isNumber),
            !uid[uid.index(after: marker)...].isEmpty
        else { return nil }
        return Int(number)
    }

    public init(pid: UInt32, name: String, uid: String, label: String, state: String) {
        self.pid = pid
        self.name = name
        self.uid = uid
        self.label = label
        self.state = state
    }
}

/// The activity manager's process list: what `dumpsys activity lru` prints,
/// one line to a process ("#27: cch  + 5 CEM  --------- 1550:org.example/u0a41").
public enum AndroidProcessList {
    public static let command = "dumpsys activity lru"

    public static func parse(_ text: String) -> [AndroidProcess] {
        text.split(whereSeparator: \.isNewline).compactMap { parse(line: String($0)) }
    }

    private static func parse(line: String) -> AndroidProcess? {
        let tokens = line.split(whereSeparator: { $0 == " " || $0 == "\t" }).map(String.init)
        guard let first = tokens.first, first.hasPrefix("#") else { return nil }
        // "#29:" and, below ten, "# 9:".
        var index = first == "#" ? 2 : 1
        guard index < tokens.count else { return nil }
        let label = tokens[index]
        index += 1
        // The label of a process carries its place in its importance level,
        // "+15" or, below ten, "+ 5".
        if index < tokens.count, isOffset(tokens[index]) {
            index += 1
        } else if index + 1 < tokens.count, tokens[index] == "+" || tokens[index] == "-", isNumber(tokens[index + 1]) {
            index += 2
        }
        guard index < tokens.count else { return nil }
        let state = tokens[index]
        index += 1
        for token in tokens[index...] {
            if let identity = identity(token) {
                return AndroidProcess(pid: identity.pid, name: identity.name, uid: identity.uid, label: label, state: state)
            }
        }
        return nil
    }

    private static func isOffset(_ token: String) -> Bool {
        guard let sign = token.first, sign == "+" || sign == "-" else { return false }
        return isNumber(String(token.dropFirst()))
    }

    private static func isNumber(_ token: String) -> Bool {
        !token.isEmpty && token.allSatisfy(\.isNumber)
    }

    /// "2240:org.example/u0a75".
    private static func identity(_ token: String) -> (pid: UInt32, name: String, uid: String)? {
        guard let colon = token.firstIndex(of: ":"), let slash = token.lastIndex(of: "/"), colon < slash else { return nil }
        let digits = token[token.startIndex..<colon]
        guard !digits.isEmpty, digits.allSatisfy(\.isNumber), let pid = UInt32(digits) else { return nil }
        let name = String(token[token.index(after: colon)..<slash])
        let uid = String(token[token.index(after: slash)...])
        guard !name.isEmpty, !uid.isEmpty else { return nil }
        return (pid, name, uid)
    }
}

/// A command in the guest's shell and what it printed.
public struct GuestShellOutput: Equatable, Sendable {
    public var text: String
    public var exitCode: Int?

    public init(text: String, exitCode: Int?) {
        self.text = text
        self.exitCode = exitCode
    }
}

/// Runs short commands in the guest's shell. A failure to reach the guest
/// throws.
public protocol GuestShell: Sendable {
    func run(_ command: String, limit: Duration) async throws -> GuestShellOutput
}

/// How a try to free graphics memory by ending an app came out.
public enum GraphicsReclaimResult: Equatable, Sendable {
    /// The app was asked to end: its package, its process and the bytes the
    /// host had credited to it.
    case ended(package: String, pid: UInt32, bytes: UInt64)
    /// No holder is a cached app the host could name exactly.
    case nothingToEnd
    /// The guest could not be asked, or refused.
    case failed
}

/// Frees the graphics memory budget by ending the cached background app that
/// holds the most of it, only when the budget is nearly full.
///
/// The host gives the guest a fixed graphics budget. A WebGL page or a game
/// that goes to the background keeps its textures, and Android neither trims
/// them nor ends the app, because its own limits watch the guest's RAM and the
/// graphics memory is not in it. The runtime says which guest processes hold
/// how much of the budget; this ends the one a cached app holds, and no other:
///
/// - It acts when what the guest's contexts hold, with the host's smallest
///   overhead seen, stays within `nearFullMargin` of the budget for
///   `confirmingReads` readings in a row, or at once after the guest was
///   refused. Memory the host's drivers cache for reuse is not counted, so
///   churn alone does not end apps.
/// - It ends only a holder that is a cached app (`AndroidProcess.Standing`),
///   holds at least `minimumHolderBytes`, has been quiet for `minimumIdle` and
///   is named exactly: the process the guest kernel reports must be in the
///   activity manager's list under that pid and command name. Without the kernel's
///   process naming no holder has a pid and nothing is ended.
/// - The previous app comes last; the foreground, visible, perceptible and
///   home apps are never asked.
/// - At most one app ends per `killInterval` and `killsPerEpisode` in one
///   episode of pressure, which lasts until `calmReads` readings in a row
///   show none. An app that stays after it was asked is not asked again.
/// - It stands down while the guest's own low-memory killer reclaims the
///   budget (`GraphicsMemoryReading.guestReclaims`): that one decides, and
///   this starts again from nothing when it stops.
public struct GraphicsReclaim: Equatable, Sendable {
    public static let nearFullMargin: UInt64 = 96 << 20
    public static let confirmingReads = 2
    public static let minimumHolderBytes: UInt64 = 32 << 20
    public static let minimumIdle = Duration.seconds(10)
    public static let killInterval = Duration.seconds(3)
    public static let killsPerEpisode = 3
    public static let calmReads = 5
    /// After a try that ended nothing, how long to wait before asking the
    /// guest for its processes again.
    public static let retryInterval = Duration.seconds(10)

    /// Apps the policy asked to end that still held graphics memory when the
    /// guest had had time to free it.
    public private(set) var survivors: Set<UInt32> = []

    /// Readings after which an app that was asked and still holds memory is
    /// one that stays: the guest frees a context's memory within a second or
    /// two of its process ending.
    static let readingsToLeave = 2

    private var refusals: UInt64?
    private var overheadMinimum: UInt64?
    private var nearReads = 0
    private var calm = 0
    private var killsInEpisode = 0
    private var lastKill: ContinuousClock.Instant?
    private var notBefore: ContinuousClock.Instant?
    /// The apps asked to end and how many readings have passed since.
    private var asked: [UInt32: Int] = [:]

    public init() {}

    /// Starts again: a guest that restarted has no history.
    public mutating func reset() {
        self = GraphicsReclaim()
    }

    /// The holders a reclaim may end: named by a process, large enough, quiet
    /// enough and not one that stayed after being asked.
    public func candidates(in reading: GraphicsMemoryReading) -> [GraphicsHolder] {
        reading.holders.filter {
            $0.pid != 0 && $0.bytes >= Self.minimumHolderBytes && $0.idle >= Self.minimumIdle
                && !survivors.contains($0.pid) && asked[$0.pid] == nil
        }
    }

    /// Takes one reading and says whether to try to end an app now.
    public mutating func observe(_ reading: GraphicsMemoryReading, at now: ContinuousClock.Instant) -> Bool {
        guard reading.budgetBytes > 0, !reading.guestReclaims else {
            reset()
            return false
        }
        // A count that went backwards belongs to a new display.
        if let before = refusals, reading.refusals < before { reset() }
        let refused = refusals.map { reading.refusals > $0 } ?? false
        refusals = reading.refusals

        // An app asked to end that still holds memory a moment later stays.
        let listed = Set(reading.holders.map(\.pid))
        for (pid, readings) in asked {
            if !listed.contains(pid) {
                asked[pid] = nil
            } else if readings + 1 >= Self.readingsToLeave {
                survivors.insert(pid)
                asked[pid] = nil
            } else {
                asked[pid] = readings + 1
            }
        }
        survivors.formIntersection(listed)

        let overhead = reading.usageBytes > reading.contextBytes ? reading.usageBytes - reading.contextBytes : 0
        overheadMinimum = min(overheadMinimum ?? overhead, overhead)
        let live = reading.contextBytes + (overheadMinimum ?? 0)
        nearReads = live + Self.nearFullMargin >= reading.budgetBytes ? nearReads + 1 : 0

        guard refused || nearReads >= Self.confirmingReads else {
            calm += 1
            if calm >= Self.calmReads {
                killsInEpisode = 0
                survivors.removeAll()
            }
            return false
        }
        calm = 0
        if let lastKill, lastKill.duration(to: now) < Self.killInterval { return false }
        if let notBefore, now < notBefore { return false }
        if killsInEpisode >= Self.killsPerEpisode { return false }
        return !candidates(in: reading).isEmpty
    }

    /// Takes what the try came to.
    public mutating func record(_ result: GraphicsReclaimResult, at now: ContinuousClock.Instant) {
        switch result {
        case .ended(_, let pid, _):
            lastKill = now
            killsInEpisode += 1
            asked[pid] = 0
        case .nothingToEnd, .failed:
            notBefore = now + Self.retryInterval
        }
    }

    /// Whether the command name the guest kernel reports for a process agrees
    /// with the activity manager's name for it. The kernel keeps the last 15
    /// characters of a process name, so a holder agrees when its name is empty
    /// or ends the process name.
    static func namesAgree(holder: String, process: String) -> Bool {
        holder.isEmpty || process.hasSuffix(holder) || holder.hasSuffix(process)
    }

    /// The app to end: the largest candidate that is cached and exactly named,
    /// the previous app only when no cached one is.
    public func choose(from reading: GraphicsMemoryReading, processes: [AndroidProcess]) -> (holder: GraphicsHolder, process: AndroidProcess)? {
        var previous: (GraphicsHolder, AndroidProcess)?
        for holder in candidates(in: reading).sorted(by: { $0.bytes > $1.bytes }) {
            guard let process = processes.first(where: { $0.pid == holder.pid }),
                Self.namesAgree(holder: holder.name, process: process.name),
                Self.isPackageName(process.package), process.user != nil
            else { continue }
            switch process.standing {
            case .cached: return (holder, process)
            case .previous: if previous == nil { previous = (holder, process) }
            case .kept: continue
            }
        }
        return previous
    }

    /// A package name is safe to put in a shell command: it starts with a
    /// letter and has only letters, digits, dots and underscores.
    static func isPackageName(_ name: String) -> Bool {
        guard let first = name.unicodeScalars.first,
            (first.value >= 0x41 && first.value <= 0x5A) || (first.value >= 0x61 && first.value <= 0x7A)
        else { return false }
        return name.unicodeScalars.allSatisfy {
            ($0.value >= 0x30 && $0.value <= 0x39) || ($0.value >= 0x41 && $0.value <= 0x5A)
                || ($0.value >= 0x61 && $0.value <= 0x7A) || $0 == "." || $0 == "_"
        }
    }

    /// The command that ends the cached processes of a package.
    public static func killCommand(package: String, user: Int) -> String {
        "cmd activity kill --user \(user) \(package)"
    }

    /// Asks the guest which processes exist and ends the one `choose` names.
    public func reclaim(from reading: GraphicsMemoryReading, shell: some GuestShell) async -> GraphicsReclaimResult {
        let listing: GuestShellOutput
        do {
            listing = try await shell.run(AndroidProcessList.command, limit: .seconds(15))
        } catch {
            return .failed
        }
        guard listing.exitCode == 0 else { return .failed }
        guard let target = choose(from: reading, processes: AndroidProcessList.parse(listing.text)),
            let user = target.process.user
        else { return .nothingToEnd }
        do {
            let ended = try await shell.run(Self.killCommand(package: target.process.package, user: user), limit: .seconds(15))
            guard ended.exitCode == 0 else { return .failed }
        } catch {
            return .failed
        }
        return .ended(package: target.process.package, pid: target.process.pid, bytes: target.holder.bytes)
    }
}
