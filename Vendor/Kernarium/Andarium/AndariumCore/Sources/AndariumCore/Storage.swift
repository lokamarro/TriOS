// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What a guest keeps on disk, and how the application reaches it: an
/// instance directory holding the guest's choices (`settings.json`), its data
/// disk (`data.img`) and the record of the Android that last started on it
/// (`instance.json`, `Instance.swift`). The system disk is Android's, read-only,
/// and stays in the application.
public protocol GuestStorage: Sendable {
    /// Free space the host needs before a guest can be created.
    var createRequirementBytes: UInt64 { get }
    /// The guest's writable data disk, which the runtime creates at the first
    /// start and only grows afterwards.
    var dataDiskPath: String { get }
    /// The guest on disk, with its measured data, or nil if there is none.
    func load() throws -> Guest?
    func create(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws -> Guest
    /// Deletes everything the guest has: its data, its record and its
    /// choices. Afterwards `load` finds no guest, and the application is
    /// where it was before the first one was set up.
    func erase(progress: @escaping @Sendable (Double) -> Void) async throws
    /// What a start would be, from the instance on disk; nil when its record
    /// cannot be read.
    func startPlan() -> StartPlan?
    /// The data Android has written: what the data disk occupies now, which
    /// the guest changes while it runs and `load` measures once. Zero where
    /// there is no data disk.
    func measuredUsedBytes() -> UInt64
    /// Applies the update rules before a start; a refusal throws. A start
    /// whose update did not finish tries it again.
    func prepareStart(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws
    /// Puts the data back to what the update that did not finish started
    /// from, and does nothing more: the next start updates again from it.
    func restoreUpdate() throws
    /// Records the guest's choices.
    func save(_ guest: Guest) throws
}

extension GuestStorage {
    public func create(_ guest: Guest) async throws -> Guest {
        try await create(guest, progress: { _ in })
    }

    public func erase() async throws {
        try await erase(progress: { _ in })
    }

    public func prepareStart(_ guest: Guest) async throws {
        try await prepareStart(guest, progress: { _ in })
    }
}

/// Where a guest keeps its files. The paths of the data disk and of the
/// release become part of a kernel argument (see `DiskPath`), so they must be
/// ones the runtime accepts.
public struct GuestLocations: Equatable, Sendable {
    public var directory: URL

    public init(directory: URL) {
        self.directory = directory
    }

    public var data: URL { directory.appendingPathComponent(InstanceFiles.dataDisk) }
    public var settings: URL { directory.appendingPathComponent("settings.json") }
    public var instance: URL { directory.appendingPathComponent(InstanceFiles.stateFile) }
    /// What the first release kept: the system and the data in one image.
    var legacyImages: [URL] {
        [directory.appendingPathComponent("guest.img"), directory.appendingPathComponent("guest.img.partial")]
    }
}

/// Whether a host file path can name the guest's disk. The runtime passes
/// the path to Linux in a `ubd<n>=` kernel argument, which the kernel splits
/// at whitespace (its Latin-1 table, so also a byte 0xa0) and in which a
/// double quote, a comma or a colon start something else. A path carrying
/// one, or an empty one, is refused by the runtime at start; this is the same
/// rule (`darwin/device/block/ubd_path.hpp`), applied before a disk is
/// written to a place it could never boot from.
public enum DiskPath {
    public static func isUsable(_ path: String) -> Bool {
        guard !path.isEmpty else { return false }
        let refused: Set<UInt8> = [0x20, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0xa0, 0x22, 0x2c, 0x3a]
        return !path.utf8.contains { refused.contains($0) }
    }
}

public enum StorageError: Error, Equatable, LocalizedError {
    case notEnoughSpace(neededBytes: UInt64)
    case missingRelease
    /// The guest's disk or the release would sit at a path Android's kernel
    /// cannot be given.
    case unusableDiskPath(String)

    public var errorDescription: String? {
        switch self {
        case .notEnoughSpace:
            return "There is not enough free storage to set up the guest."
        case .missingRelease:
            return "This copy of Andarium does not carry a system."
        case .unusableDiskPath(let path):
            return "The guest cannot start from \(path): its name has a space, comma, colon or quotation mark."
        }
    }
}

/// A guest's files for the layout with the system apart from the data: the
/// release the application carries (`guest/system.img`, its layout, the kernel
/// command line, the manifest) and the instance's data disk. Create and Erase
/// write no disk, so they take no time; the runtime creates the data disk,
/// sized by the guest's capacity, at the first start.
public struct TwoDiskStorage: GuestStorage {
    public let locations: GuestLocations
    public let release: ReleaseIdentity
    /// The release directory inside the application.
    public let releaseDirectory: URL

    public init(locations: GuestLocations, release: ReleaseIdentity, releaseDirectory: URL) {
        self.locations = locations
        self.release = release
        self.releaseDirectory = releaseDirectory
    }

    private var files: InstanceFiles { InstanceFiles(directory: locations.directory) }

    public var createRequirementBytes: UInt64 { Limits.firstStartReserveBytes }

    public var dataDiskPath: String { locations.data.path }

    /// The data disk's size in GiB for the runtime, which creates a disk that
    /// is absent and grows one that is smaller.
    public func dataGiB(for guest: Guest) -> UInt32 {
        UInt32(clamping: guest.effectiveCapacityGiB)
    }

    public func load() throws -> Guest? {
        let manager = FileManager.default
        guard manager.fileExists(atPath: locations.settings.path) else { return nil }
        var guest = try JSONDecoder().decode(Guest.self, from: Data(contentsOf: locations.settings))
        guest.usedBytes = measuredUsedBytes()
        return guest
    }

    /// Refuses a location the runtime could not boot, before anything is
    /// written there.
    private func requireUsablePaths() throws {
        for path in [dataDiskPath, releaseDirectory.path] where !DiskPath.isUsable(path) {
            throw StorageError.unusableDiskPath(path)
        }
    }

    public func create(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws -> Guest {
        try requireUsablePaths()
        let manager = FileManager.default
        try manager.createDirectory(at: locations.directory, withIntermediateDirectories: true)
        for legacy in locations.legacyImages { try? manager.removeItem(at: legacy) }
        try files.apply(InstanceDecision(.create, "A new guest."), release: release)
        var created = guest.erased
        created.usedBytes = 0
        try save(created)
        progress(1)
        return created
    }

    public func erase(progress: @escaping @Sendable (Double) -> Void) async throws {
        try requireUsablePaths()
        // The data goes first, with the record and any snapshot: a guest that
        // cannot be read any more (an earlier layout) is erased the same way.
        // The choices go last, so a failure before them leaves the guest in
        // view for another try.
        try files.removeAll()
        let manager = FileManager.default
        for legacy in locations.legacyImages { try? manager.removeItem(at: legacy) }
        if manager.fileExists(atPath: locations.settings.path) {
            try manager.removeItem(at: locations.settings)
        }
        progress(1)
    }

    public func startPlan() -> StartPlan? {
        files.startPlan(for: release)
    }

    public func prepareStart(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws {
        try requireUsablePaths()
        var decision = try files.decide(for: release)
        if decision.action == .refusePending {
            // Start is Try Again; a release other than the one updating
            // refuses the retry with the reason.
            decision = try files.decide(for: release, flags: InstanceFlags(retry: true))
        }
        try files.apply(decision, release: release)
        progress(1)
    }

    public func restoreUpdate() throws {
        try files.apply(files.decide(for: release, flags: InstanceFlags(restoreSnapshot: true)), release: release)
    }

    /// Records that the guest reached `sys.boot_completed`, at the moment the
    /// application first sees it. A pending update counts as applied from then
    /// on: its snapshot is dropped and a newer system updates from the data as
    /// it is.
    public func bootCompleted() throws {
        try files.booted(release: release)
    }

    /// Records that the guest reached `sys.boot_completed` and shut down
    /// cleanly (`lastCompleted`).
    public func completeRun() throws {
        try files.complete(release: release)
    }

    public func save(_ guest: Guest) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try FileManager.default.createDirectory(at: locations.directory, withIntermediateDirectories: true)
        try encoder.encode(guest).write(to: locations.settings, options: .atomic)
        applyBackupPolicy(guest)
    }

    /// The data Android has written: what the data disk occupies. The system
    /// is not in it.
    public func measuredUsedBytes() -> UInt64 {
        // A URL caches the resource values it has read, and the disk changes
        // under it while the guest runs, so each measure starts from the path.
        let values = try? URL(fileURLWithPath: locations.data.path).resourceValues(forKeys: [.fileAllocatedSizeKey])
        return UInt64(values?.fileAllocatedSize ?? 0)
    }

    /// iCloud and computer backups include the guest's files only when the
    /// user chose that for the guest. The flag sits on the guest's directory,
    /// which covers whatever is later put in it (the data disk, a snapshot),
    /// and on its files.
    private func applyBackupPolicy(_ guest: Guest) {
        var excluded = URLResourceValues()
        excluded.isExcludedFromBackup = true
        var included = URLResourceValues()
        included.isExcludedFromBackup = false
        for url in [locations.directory, locations.data, locations.instance, locations.settings] {
            var target = url
            try? target.setResourceValues(excluded)
            // On a Mac the backup service keeps the exclusion of a path and
            // restores it on a file that replaced the original, as the
            // atomic write of the settings does. A clear finds no flag on the
            // new file and leaves that record in place, so the flag is set
            // first and the clear then removes both.
            if guest.includeInBackups { try? target.setResourceValues(included) }
        }
    }
}
