// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// A guest instance of the Android release: the data disk (`data.img`) and
/// `instance.json`, which says which release the disk was last started with.
/// The rules are those of `android/lineage-24/README.md#guest-instance-storage`;
/// `tools/android_instance.py` is the reference implementation and
/// `support/instance/update-decisions.json` holds the vectors both reproduce.

public enum InstanceError: Error, Equatable, LocalizedError {
    case release(String)
    case state(String)
    case refused(String)
    case cannotClone(String)

    public var errorDescription: String? {
        switch self {
        case .release(let message), .state(let message), .refused(let message): message
        case .cannotClone(let message): "The guest's data cannot be snapshotted (it needs an APFS volume): \(message)"
        }
    }
}

/// What an instance records about the release it starts with, read from the
/// release's manifest (schema 6) and its partition layout (schema 2).
public struct ReleaseIdentity: Equatable, Sendable {
    public var releaseID: String
    public var buildFingerprint: String
    public var buildNumber: Int
    public var securityPatch: String
    public var systemDiskSHA256: String
    public var dataLayout: Int
    public var minimumGiB: Int
    public var defaultGiB: Int
    public var maximumGiB: Int

    public init(
        releaseID: String, buildFingerprint: String, buildNumber: Int, securityPatch: String,
        systemDiskSHA256: String, dataLayout: Int, minimumGiB: Int, defaultGiB: Int, maximumGiB: Int
    ) {
        self.releaseID = releaseID
        self.buildFingerprint = buildFingerprint
        self.buildNumber = buildNumber
        self.securityPatch = securityPatch
        self.systemDiskSHA256 = systemDiskSHA256
        self.dataLayout = dataLayout
        self.minimumGiB = minimumGiB
        self.defaultGiB = defaultGiB
        self.maximumGiB = maximumGiB
    }

    /// The identity from a release's `release-manifest.json` and
    /// `guest/partition-layout.json`. It is never computed by hashing the
    /// disk at launch.
    public init(manifest: Data, layout: Data) throws {
        func object(_ data: Data, _ label: String) throws -> [String: Any] {
            guard let value = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                throw InstanceError.release("The release's \(label) cannot be read.")
            }
            return value
        }
        func integer(_ value: Any?) -> Int? {
            guard let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID() else { return nil }
            let result = number.intValue
            return result >= 1 ? result : nil
        }
        let manifest = try object(manifest, "manifest")
        let layout = try object(layout, "partition layout")
        guard
            let id = manifest["release_id"] as? String, Self.releaseIDIsValid(id),
            let android = manifest["android"] as? [String: Any],
            let fingerprint = android["fingerprint"] as? String, !fingerprint.isEmpty,
            let buildNumber = integer(android["build_number"]),
            let patch = android["security_patch"] as? String,
            let system = layout["system_disk"] as? [String: Any],
            let sha = system["sha256"] as? String, Self.sha256IsValid(sha),
            let data = layout["data_disk"] as? [String: Any],
            let dataLayout = integer(data["layout_version"]),
            let capacity = data["capacity_gib"] as? [String: Any],
            let minimum = integer(capacity["minimum"]),
            let standard = integer(capacity["default"]),
            let maximum = integer(capacity["maximum"])
        else {
            throw InstanceError.release("This copy of Andarium carries a system release without a usable identity.")
        }
        self.init(
            releaseID: id, buildFingerprint: fingerprint, buildNumber: buildNumber, securityPatch: patch,
            systemDiskSHA256: sha, dataLayout: dataLayout, minimumGiB: minimum, defaultGiB: standard,
            maximumGiB: maximum
        )
    }

    static func releaseIDIsValid(_ id: String) -> Bool {
        id.range(of: "^[a-z0-9][a-z0-9._-]{0,63}$", options: .regularExpression) != nil
    }

    static func sha256IsValid(_ text: String) -> Bool {
        text.range(of: "^[0-9a-f]{64}$", options: .regularExpression) != nil
    }
}

/// `instance.json`, schema 1.
public struct InstanceState: Equatable, Sendable {
    public struct Completed: Equatable, Sendable, Codable {
        public var releaseID: String
        public var buildIncremental: String
        public var at: String

        private enum CodingKeys: String, CodingKey {
            case releaseID = "release_id"
            case buildIncremental = "build_incremental"
            case at
        }
    }

    /// The identity the disk was last started with before an update.
    public struct Origin: Equatable, Sendable, Codable {
        public var releaseID: String
        public var buildFingerprint: String
        public var buildIncremental: String
        public var systemDiskSHA256: String
        public var dataLayout: Int
        public var lastBooted: String

        private enum CodingKeys: String, CodingKey {
            case releaseID = "release_id"
            case buildFingerprint = "build_fingerprint"
            case buildIncremental = "build_incremental"
            case systemDiskSHA256 = "system_disk_sha256"
            case dataLayout = "data_layout"
            case lastBooted = "last_booted"
        }
    }

    /// An update whose first start has not reached `sys.boot_completed`.
    public struct Pending: Equatable, Sendable, Codable {
        public var toReleaseID: String
        public var snapshot: String
        public var from: Origin

        private enum CodingKeys: String, CodingKey {
            case toReleaseID = "to_release_id"
            case snapshot
            case from
        }
    }

    public var dataLayout: Int
    public var releaseID: String
    public var buildFingerprint: String
    public var buildIncremental: String
    public var systemDiskSHA256: String
    public var createdWith: String
    public var lastBooted: String
    public var lastCompleted: Completed?
    public var pendingUpdate: Pending?

    public init(
        dataLayout: Int, releaseID: String, buildFingerprint: String, buildIncremental: String,
        systemDiskSHA256: String, createdWith: String, lastBooted: String,
        lastCompleted: Completed? = nil, pendingUpdate: Pending? = nil
    ) {
        self.dataLayout = dataLayout
        self.releaseID = releaseID
        self.buildFingerprint = buildFingerprint
        self.buildIncremental = buildIncremental
        self.systemDiskSHA256 = systemDiskSHA256
        self.createdWith = createdWith
        self.lastBooted = lastBooted
        self.lastCompleted = lastCompleted
        self.pendingUpdate = pendingUpdate
    }

    public var buildNumber: Int { Int(buildIncremental) ?? 0 }

    /// A new instance created for `release` at `when`.
    public init(creating release: ReleaseIdentity, at when: String) {
        self.init(
            dataLayout: release.dataLayout, releaseID: release.releaseID, buildFingerprint: release.buildFingerprint,
            buildIncremental: String(release.buildNumber), systemDiskSHA256: release.systemDiskSHA256,
            createdWith: release.releaseID, lastBooted: when
        )
    }

    static let keys: Set<String> = [
        "schema", "data_layout", "release_id", "build_fingerprint", "build_incremental", "system_disk_sha256",
        "created_with", "last_booted", "last_completed", "pending_update",
    ]
}

extension InstanceState: Codable {
    private enum CodingKeys: String, CodingKey {
        case schema
        case dataLayout = "data_layout"
        case releaseID = "release_id"
        case buildFingerprint = "build_fingerprint"
        case buildIncremental = "build_incremental"
        case systemDiskSHA256 = "system_disk_sha256"
        case createdWith = "created_with"
        case lastBooted = "last_booted"
        case lastCompleted = "last_completed"
        case pendingUpdate = "pending_update"
    }

    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        guard try values.decode(Int.self, forKey: .schema) == 1 else {
            throw InstanceError.state("This guest's instance.json is of a schema Andarium does not know.")
        }
        let build = try values.decode(String.self, forKey: .buildIncremental)
        guard build.range(of: "^[1-9][0-9]{0,9}$", options: .regularExpression) != nil else {
            throw InstanceError.state("This guest's instance.json has an invalid build number.")
        }
        self.init(
            dataLayout: try values.decode(Int.self, forKey: .dataLayout),
            releaseID: try values.decode(String.self, forKey: .releaseID),
            buildFingerprint: try values.decode(String.self, forKey: .buildFingerprint),
            buildIncremental: build,
            systemDiskSHA256: try values.decode(String.self, forKey: .systemDiskSHA256),
            createdWith: try values.decode(String.self, forKey: .createdWith),
            lastBooted: try values.decode(String.self, forKey: .lastBooted),
            lastCompleted: try values.decodeIfPresent(Completed.self, forKey: .lastCompleted),
            pendingUpdate: try values.decodeIfPresent(Pending.self, forKey: .pendingUpdate)
        )
    }

    /// The two optional fields are written as `null`, as the schema has them.
    public func encode(to encoder: Encoder) throws {
        var values = encoder.container(keyedBy: CodingKeys.self)
        try values.encode(1, forKey: .schema)
        try values.encode(dataLayout, forKey: .dataLayout)
        try values.encode(releaseID, forKey: .releaseID)
        try values.encode(buildFingerprint, forKey: .buildFingerprint)
        try values.encode(buildIncremental, forKey: .buildIncremental)
        try values.encode(systemDiskSHA256, forKey: .systemDiskSHA256)
        try values.encode(createdWith, forKey: .createdWith)
        try values.encode(lastBooted, forKey: .lastBooted)
        if let lastCompleted { try values.encode(lastCompleted, forKey: .lastCompleted) } else { try values.encodeNil(forKey: .lastCompleted) }
        if let pendingUpdate { try values.encode(pendingUpdate, forKey: .pendingUpdate) } else { try values.encodeNil(forKey: .pendingUpdate) }
    }
}

public enum InstanceAction: String, Equatable, Sendable {
    case create, boot, update, migrate, retry, restore, reset
    case refuseDowngrade = "refuse_downgrade"
    case refuseConflict = "refuse_conflict"
    case refusePending = "refuse_pending"

    public var isRefusal: Bool {
        switch self {
        case .refuseDowngrade, .refuseConflict, .refusePending: true
        default: false
        }
    }
}

public struct InstanceFlags: Equatable, Sendable {
    public var retry = false
    public var restoreSnapshot = false
    public var reset = false

    public init(retry: Bool = false, restoreSnapshot: Bool = false, reset: Bool = false) {
        self.retry = retry
        self.restoreSnapshot = restoreSnapshot
        self.reset = reset
    }
}

public struct InstanceDecision: Equatable, Sendable {
    public var action: InstanceAction
    public var reason: String

    public init(_ action: InstanceAction, _ reason: String) {
        self.action = action
        self.reason = reason
    }
}

public enum InstanceRules {
    /// What to do before starting `release` on an instance. Pure: it looks at
    /// nothing else, so every implementation reproduces it from the vectors.
    public static func decide(
        state: InstanceState?, dataDiskExists: Bool, snapshotExists: Bool,
        release: ReleaseIdentity, flags: InstanceFlags = InstanceFlags()
    ) -> InstanceDecision {
        if [flags.retry, flags.restoreSnapshot, flags.reset].filter({ $0 }).count > 1 {
            return InstanceDecision(.refuseConflict, "Retry, restore and reset exclude each other.")
        }
        if flags.reset {
            return InstanceDecision(.reset, "The guest is deleted and created anew.")
        }
        guard let state else {
            if dataDiskExists {
                return InstanceDecision(.refuseConflict, "The guest's data has no record of which system wrote it.")
            }
            return InstanceDecision(.create, "A new guest.")
        }
        if let pending = state.pendingUpdate {
            if flags.restoreSnapshot {
                guard snapshotExists else {
                    return InstanceDecision(.refuseConflict, "The update's snapshot is missing.")
                }
                return InstanceDecision(.restore, "The data the update started from is put back.")
            }
            if flags.retry {
                guard release.releaseID == pending.toReleaseID, dataDiskExists else {
                    return InstanceDecision(.refuseConflict, "A retry starts the system whose update did not complete.")
                }
                return InstanceDecision(.retry, "The update starts again, its snapshot kept.")
            }
            if release.releaseID != state.releaseID, release.buildNumber > state.buildNumber {
                guard snapshotExists else {
                    return InstanceDecision(.refuseConflict, "The update's snapshot is missing.")
                }
                if release.dataLayout < pending.from.dataLayout {
                    return InstanceDecision(.refuseDowngrade, "The release's data layout is older than the snapshot's.")
                }
                return InstanceDecision(
                    release.dataLayout == pending.from.dataLayout ? .update : .migrate,
                    "The unfinished update's data is restored before starting the newer system."
                )
            }
            return InstanceDecision(
                .refusePending,
                "The start of \(pending.toReleaseID) never finished: retry it, restore its snapshot, or erase the guest."
            )
        }
        if flags.retry || flags.restoreSnapshot {
            return InstanceDecision(.refuseConflict, "No update is pending.")
        }
        if !dataDiskExists {
            return InstanceDecision(.create, "The guest has no data disk.")
        }
        if release.releaseID == state.releaseID {
            if release.systemDiskSHA256 != state.systemDiskSHA256 {
                return InstanceDecision(.refuseConflict, "One system release names two system disks.")
            }
            return InstanceDecision(.boot, "The system the data was last started with.")
        }
        if release.buildNumber < state.buildNumber {
            return InstanceDecision(
                .refuseDowngrade,
                "Build \(release.buildNumber) is older than build \(state.buildNumber), which the data was started with: update the system, or erase the guest."
            )
        }
        if release.buildNumber == state.buildNumber {
            return InstanceDecision(.refuseConflict, "Two system releases share one build number.")
        }
        if release.dataLayout == state.dataLayout {
            return InstanceDecision(.update, "A newer system on the same data layout.")
        }
        if release.dataLayout > state.dataLayout {
            return InstanceDecision(.migrate, "A newer system on a newer data layout.")
        }
        return InstanceDecision(.refuseDowngrade, "The release's data layout is older than the data's.")
    }
}

/// An instance's files: `data.img`, `instance.json` and the snapshot of an
/// update, in one directory.
public struct InstanceFiles: Sendable {
    public static let dataDisk = "data.img"
    public static let stateFile = "instance.json"
    public static let snapshotPrefix = "data.img.pre-"

    public var directory: URL

    public init(directory: URL) {
        self.directory = directory
    }

    public var data: URL { directory.appendingPathComponent(Self.dataDisk) }
    public var state: URL { directory.appendingPathComponent(Self.stateFile) }

    public func snapshot(for releaseID: String) -> URL {
        directory.appendingPathComponent(Self.snapshotPrefix + releaseID)
    }

    public func readState() throws -> InstanceState? {
        guard FileManager.default.fileExists(atPath: state.path) else { return nil }
        do {
            return try JSONDecoder().decode(InstanceState.self, from: Data(contentsOf: state))
        } catch let error as InstanceError {
            throw error
        } catch {
            throw InstanceError.state("This guest's instance.json cannot be read: \(error.localizedDescription)")
        }
    }

    /// Replaces `instance.json` atomically.
    public func write(_ value: InstanceState) throws {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(value).write(to: state, options: .atomic)
    }

    /// The decision's inputs as the files have them.
    public func decide(for release: ReleaseIdentity, flags: InstanceFlags = InstanceFlags()) throws -> InstanceDecision {
        let state = try readState()
        let manager = FileManager.default
        let snapshotExists = state?.pendingUpdate.map { manager.fileExists(atPath: directory.appendingPathComponent($0.snapshot).path) } ?? false
        return InstanceRules.decide(
            state: state, dataDiskExists: manager.fileExists(atPath: data.path), snapshotExists: snapshotExists,
            release: release, flags: flags
        )
    }

    /// An APFS clone: instant, and it costs only the blocks that diverge.
    private func clone(_ source: URL, to destination: URL) throws {
        let temporary = destination.appendingPathExtension("tmp")
        try? FileManager.default.removeItem(at: temporary)
        guard clonefile(source.path, temporary.path, 0) == 0 else {
            throw InstanceError.cannotClone(String(cString: strerror(errno)))
        }
        guard rename(temporary.path, destination.path) == 0 else {
            let message = String(cString: strerror(errno))
            try? FileManager.default.removeItem(at: temporary)
            throw InstanceError.cannotClone(message)
        }
    }

    public static func timestamp(_ date: Date = Date()) -> String {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime]
        formatter.timeZone = TimeZone(identifier: "UTC")
        return formatter.string(from: date)
    }

    /// Deletes the data disk, the record and every snapshot, trying each of
    /// them; the first failure is thrown once the rest are done.
    public func removeAll() throws {
        let manager = FileManager.default
        var doomed = [data, state]
        for entry in (try? manager.contentsOfDirectory(atPath: directory.path)) ?? []
        where entry.hasPrefix(Self.snapshotPrefix) {
            doomed.append(directory.appendingPathComponent(entry))
        }
        var failure: Error?
        for url in doomed where manager.fileExists(atPath: url.path) {
            do { try manager.removeItem(at: url) } catch { failure = failure ?? error }
        }
        if let failure { throw failure }
    }

    /// Carries out `decision` before the guest starts and returns the new
    /// state. Every step leaves the instance in a state `decide` handles: the
    /// snapshot exists before `instance.json` records it.
    @discardableResult
    public func apply(_ decision: InstanceDecision, release: ReleaseIdentity, now: Date = Date()) throws -> InstanceState {
        let manager = FileManager.default
        let when = Self.timestamp(now)
        var action = decision.action
        if action.isRefusal { throw InstanceError.refused(decision.reason) }
        if action == .reset {
            try? removeAll()
            action = .create
        }
        if action == .restore {
            guard let current = try readState(), let pending = current.pendingUpdate else {
                throw InstanceError.refused("No update is pending.")
            }
            let snapshot = directory.appendingPathComponent(pending.snapshot)
            try clone(snapshot, to: data)
            var restored = InstanceState(
                dataLayout: pending.from.dataLayout, releaseID: pending.from.releaseID,
                buildFingerprint: pending.from.buildFingerprint, buildIncremental: pending.from.buildIncremental,
                systemDiskSHA256: pending.from.systemDiskSHA256, createdWith: current.createdWith,
                lastBooted: pending.from.lastBooted
            )
            restored.lastCompleted = current.lastCompleted
            try write(restored)
            try? manager.removeItem(at: snapshot)
            // The data is back and nothing else happens: the next start
            // decides afresh from the restored record, which for the release
            // that was updating is the update again.
            return restored
        }
        if action == .create {
            try manager.createDirectory(at: directory, withIntermediateDirectories: true)
            try? manager.removeItem(at: data)
            let created = InstanceState(creating: release, at: when)
            try write(created)
            return created
        }
        guard var current = try readState() else { throw InstanceError.state("This guest has no instance.json.") }
        if action == .boot || action == .retry {
            current.lastBooted = when
            try write(current)
            return current
        }
        precondition(action == .update || action == .migrate)
        if current.pendingUpdate != nil {
            try apply(InstanceDecision(.restore, "Restore the data before the unfinished update."), release: release, now: now)
            return try apply(decide(for: release), release: release, now: now)
        }
        if action == .migrate {
            // No data layout other than 1 exists, so there is no migration.
            throw InstanceError.refused("This system needs a newer data layout than Andarium can migrate to; erase the guest.")
        }
        let name = Self.snapshotPrefix + release.releaseID
        try clone(data, to: directory.appendingPathComponent(name))
        let pending = InstanceState.Pending(
            toReleaseID: release.releaseID, snapshot: name,
            from: InstanceState.Origin(
                releaseID: current.releaseID, buildFingerprint: current.buildFingerprint,
                buildIncremental: current.buildIncremental, systemDiskSHA256: current.systemDiskSHA256,
                dataLayout: current.dataLayout, lastBooted: current.lastBooted
            )
        )
        let updated = InstanceState(
            dataLayout: current.dataLayout, releaseID: release.releaseID, buildFingerprint: release.buildFingerprint,
            buildIncremental: String(release.buildNumber), systemDiskSHA256: release.systemDiskSHA256,
            createdWith: current.createdWith, lastBooted: when, lastCompleted: current.lastCompleted,
            pendingUpdate: pending
        )
        try write(updated)
        return updated
    }

    /// Records that a start of `release` reached `sys.boot_completed`. Call it
    /// at the moment the host first sees the guest boot. A pending update
    /// counts as applied from then on: it ends, its snapshot is deleted, and a
    /// newer system updates from the data as it is, with a snapshot of its
    /// own. Restoring the snapshot is for an update that never booted. An
    /// instance with no pending update is left as it is.
    @discardableResult
    public func booted(release: ReleaseIdentity) throws -> InstanceState {
        guard var current = try readState(), current.releaseID == release.releaseID else {
            throw InstanceError.state("The guest was not started with this system.")
        }
        guard let snapshot = current.pendingUpdate?.snapshot else { return current }
        current.pendingUpdate = nil
        try write(current)
        try? FileManager.default.removeItem(at: directory.appendingPathComponent(snapshot))
        return current
    }

    /// Records that `release` reached `sys.boot_completed` and the guest shut
    /// down cleanly (`lastCompleted`). A pending update that `booted` did not
    /// end is over: its snapshot is deleted.
    @discardableResult
    public func complete(release: ReleaseIdentity, now: Date = Date()) throws -> InstanceState {
        guard var current = try readState(), current.releaseID == release.releaseID else {
            throw InstanceError.state("The guest was not started with this system.")
        }
        let snapshot = current.pendingUpdate?.snapshot
        current.lastCompleted = InstanceState.Completed(
            releaseID: release.releaseID, buildIncremental: String(release.buildNumber), at: Self.timestamp(now)
        )
        current.pendingUpdate = nil
        try write(current)
        if let snapshot { try? FileManager.default.removeItem(at: directory.appendingPathComponent(snapshot)) }
        return current
    }
}
