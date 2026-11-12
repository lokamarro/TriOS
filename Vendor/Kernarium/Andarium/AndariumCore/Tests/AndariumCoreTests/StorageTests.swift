// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

private func releaseIdentity(
    _ id: String = "20261031-los24-r4", build: Int = 100, sha: String = String(repeating: "a", count: 64)
) -> ReleaseIdentity {
    ReleaseIdentity(
        releaseID: id, buildFingerprint: "fp-\(id)", buildNumber: build, securityPatch: "2026-10-01",
        systemDiskSHA256: sha, dataLayout: 1, minimumGiB: 4, defaultGiB: 8, maximumGiB: 64
    )
}

private func makeStorage(release: ReleaseIdentity = releaseIdentity()) throws -> (TwoDiskStorage, URL) {
    let root = FileManager.default.temporaryDirectory
        .appendingPathComponent("andarium-core-tests-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    let storage = TwoDiskStorage(
        locations: GuestLocations(directory: root.appendingPathComponent("Guest")),
        release: release, releaseDirectory: root.appendingPathComponent("andarium-system")
    )
    return (storage, root)
}

private let host = HostDevice(
    name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
)

struct StorageTests {
    @Test func aStorageWithNothingInItHasNoGuest() throws {
        let (storage, root) = try makeStorage()
        defer { try? FileManager.default.removeItem(at: root) }
        #expect(try storage.load() == nil)
    }

    @Test func createWritesTheSettingsAndTheInstanceButNoDisk() async throws {
        let (storage, root) = try makeStorage()
        defer { try? FileManager.default.removeItem(at: root) }

        let created = try await storage.create(Guest.fresh(on: host))
        #expect(created.usedBytes == 0)
        // The runtime creates the data disk at the first start.
        #expect(!FileManager.default.fileExists(atPath: storage.locations.data.path))
        let state = try #require(try InstanceFiles(directory: storage.locations.directory).readState())
        #expect(state.releaseID == "20261031-los24-r4" && state.createdWith == "20261031-los24-r4")
        #expect(try storage.load()?.cores == created.cores)
        #expect(storage.dataGiB(for: created) == UInt32(created.effectiveCapacityGiB))
    }

    @Test func theDataIsWhatTheDataDiskOccupies() async throws {
        let (storage, root) = try makeStorage()
        defer { try? FileManager.default.removeItem(at: root) }
        _ = try await storage.create(Guest.fresh(on: host))
        try Data(repeating: 0x5A, count: 3 * 1024 * 1024).write(to: storage.locations.data)
        let used = try #require(try storage.load()?.usedBytes)
        #expect(used >= 3 * 1024 * 1024 && used < 8 * 1024 * 1024)
    }

    /// The figure Home shows after a run: the measure follows the disk as the
    /// guest writes to it, with no reload of the guest.
    @Test func theMeasureFollowsTheDataDiskAsItGrows() async throws {
        let (storage, root) = try makeStorage()
        defer { try? FileManager.default.removeItem(at: root) }
        _ = try await storage.create(Guest.fresh(on: host))
        #expect(storage.measuredUsedBytes() == 0)
        let handle = try { () -> FileHandle in
            FileManager.default.createFile(atPath: storage.locations.data.path, contents: Data(repeating: 1, count: 1 << 20))
            return try FileHandle(forWritingTo: storage.locations.data)
        }()
        defer { try? handle.close() }
        let first = storage.measuredUsedBytes()
        #expect(first >= 1 << 20 && first < 4 << 20)
        try handle.seekToEnd()
        try handle.write(contentsOf: Data(repeating: 2, count: 8 << 20))
        try handle.synchronize()
        let second = storage.measuredUsedBytes()
        #expect(second >= first + (8 << 20))
    }

    /// Start in recovery waits for the first start: the plan is the first
    /// start until the runtime has created the data disk, and an erased and
    /// recreated guest is back to it.
    @Test func theFirstStartLastsUntilTheDataDiskExistsAndAnEraseReturnsToIt() async throws {
        let (storage, root) = try makeStorage()
        defer { try? FileManager.default.removeItem(at: root) }
        let guest = try await storage.create(Guest.fresh(on: host))
        #expect(storage.startPlan() == .firstStart)
        #expect(storage.startPlan()?.offersRecovery == false)
        // The first start is prepared and under way; the runtime creates the
        // disk and Android sets itself up.
        try await storage.prepareStart(guest)
        #expect(storage.startPlan() == .firstStart)
        try Data("android data".utf8).write(to: storage.locations.data)
        #expect(storage.startPlan() == .normal)
        #expect(storage.startPlan()?.offersRecovery == true)
        // The plan lasts across a clean shutdown and a new process.
        try storage.bootCompleted()
        try storage.completeRun()
        #expect(storage.startPlan()?.offersRecovery == true)

        try await storage.erase()
        _ = try await storage.create(Guest.fresh(on: host))
        #expect(storage.startPlan() == .firstStart)
        #expect(storage.startPlan()?.offersRecovery == false)
    }

    @Test func eraseDeletesTheDataTheRecordAndTheChoicesSoNoGuestIsLeft() async throws {
        let (storage, root) = try makeStorage()
        defer { try? FileManager.default.removeItem(at: root) }
        var guest = Guest.fresh(on: host)
        guest.cores = 3
        let created = try await storage.create(guest)
        try Data(repeating: 1, count: 4096).write(to: storage.locations.data)
        // A snapshot an update left is erased with the rest.
        let snapshot = storage.locations.directory.appendingPathComponent(InstanceFiles.snapshotPrefix + "r2")
        try Data(repeating: 2, count: 4096).write(to: snapshot)
        #expect(try storage.load()?.cores == 3 && created.cores == 3)

        try await storage.erase()
        let manager = FileManager.default
        #expect(!manager.fileExists(atPath: storage.locations.data.path))
        #expect(!manager.fileExists(atPath: storage.locations.settings.path))
        #expect(!manager.fileExists(atPath: storage.locations.instance.path))
        #expect(!manager.fileExists(atPath: snapshot.path))
        // The next launch finds no guest, so it shows the first-launch screen,
        // and a guest set up afterwards starts from the defaults.
        #expect(try storage.load() == nil)
        #expect(storage.startPlan() == .firstStart)
        let next = try await storage.create(Guest.fresh(on: host))
        #expect(next.cores == host.processorCount)
    }

    @Test func eraseWithNoGuestChangesNothing() async throws {
        let (storage, root) = try makeStorage()
        defer { try? FileManager.default.removeItem(at: root) }
        try await storage.erase()
        #expect(try storage.load() == nil)
    }

    @Test func aGuestOfTheFirstReleaseIsErasedLikeAnyOtherAndItsImageGoes() async throws {
        let (storage, root) = try makeStorage()
        defer { try? FileManager.default.removeItem(at: root) }
        let directory = storage.locations.directory
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        // The first release kept one image and an instance.json of schema 0.
        try Data(count: 4096).write(to: directory.appendingPathComponent("guest.img"))
        try Data(#"{"schema": 0, "data_layout": "single-image"}"#.utf8).write(to: storage.locations.instance)
        #expect(throws: InstanceError.self) { try storage.completeRun() }
        await #expect(throws: InstanceError.self) { try await storage.prepareStart(Guest.fresh(on: host)) }

        try await storage.erase()
        #expect(!FileManager.default.fileExists(atPath: directory.appendingPathComponent("guest.img").path))
        #expect(try InstanceFiles(directory: directory).readState() == nil)
        // A guest set up afterwards records the current release.
        _ = try await storage.create(Guest.fresh(on: host))
        #expect(try InstanceFiles(directory: directory).readState()?.releaseID == "20261031-los24-r4")
    }

    @Test func startingAppliesTheUpdateRules() async throws {
        let (first, root) = try makeStorage()
        defer { try? FileManager.default.removeItem(at: root) }
        let guest = try await first.create(Guest.fresh(on: host))
        // The first start finds no data disk and keeps the new instance; the
        // runtime then creates the disk.
        try await first.prepareStart(guest)
        try Data("android data".utf8).write(to: first.locations.data)
        try await first.prepareStart(guest)
        try first.completeRun()
        #expect(try InstanceFiles(directory: first.locations.directory).readState()?.lastCompleted != nil)

        // A newer release snapshots the data first.
        let newer = TwoDiskStorage(
            locations: first.locations, release: releaseIdentity("20261102-los24-r5", build: 200, sha: String(repeating: "b", count: 64)),
            releaseDirectory: first.releaseDirectory
        )
        try await newer.prepareStart(guest)
        #expect(FileManager.default.fileExists(atPath: InstanceFiles(directory: first.locations.directory).snapshot(for: "20261102-los24-r5").path))
        // Until it completes, a start tries the update again, and the older
        // release is refused.
        try await newer.prepareStart(guest)
        #expect(newer.startPlan() == .unfinishedUpdate(to: "20261102-los24-r5", canRetry: true, canRestore: true))
        try newer.completeRun()
        await #expect(throws: InstanceError.self) { try await first.prepareStart(guest) }
    }

    @Test func startTriesAnUnfinishedUpdateAgainAndRestoreOnlyPutsTheDataBack() async throws {
        let (first, root) = try makeStorage()
        defer { try? FileManager.default.removeItem(at: root) }
        let guest = try await first.create(Guest.fresh(on: host))
        try await first.prepareStart(guest)
        try Data("android data".utf8).write(to: first.locations.data)
        try first.completeRun()

        let newer = TwoDiskStorage(
            locations: first.locations, release: releaseIdentity("20261102-los24-r5", build: 200, sha: String(repeating: "b", count: 64)),
            releaseDirectory: first.releaseDirectory
        )
        #expect(newer.startPlan() == .update(from: "20261031-los24-r4", to: "20261102-los24-r5"))
        try await newer.prepareStart(guest)
        // The update's start never completed; Android wrote to the data.
        try Data("android data, upgraded".utf8).write(to: first.locations.data)
        #expect(newer.startPlan() == .unfinishedUpdate(to: "20261102-los24-r5", canRetry: true, canRestore: true))

        // Start tries it again, keeping the snapshot and the data as they are.
        try await newer.prepareStart(guest)
        #expect(try String(contentsOf: first.locations.data, encoding: .utf8) == "android data, upgraded")
        #expect(newer.startPlan() == .unfinishedUpdate(to: "20261102-los24-r5", canRetry: true, canRestore: true))

        // Restore puts back the data the update started from and nothing
        // more: no snapshot, no pending update, the update waiting to start.
        try newer.restoreUpdate()
        #expect(try String(contentsOf: first.locations.data, encoding: .utf8) == "android data")
        #expect(newer.startPlan() == .update(from: "20261031-los24-r4", to: "20261102-los24-r5"))
        #expect(!FileManager.default.fileExists(atPath: InstanceFiles(directory: first.locations.directory).snapshot(for: "20261102-los24-r5").path))
        // Nothing is left to restore, and the next start updates afresh.
        #expect(throws: InstanceError.self) { try newer.restoreUpdate() }
        try await newer.prepareStart(guest)
        #expect(newer.startPlan() == .unfinishedUpdate(to: "20261102-los24-r5", canRetry: true, canRestore: true))

        // Completing ends it.
        try newer.completeRun()
        #expect(newer.startPlan() == .normal)
    }

    @Test func erasingIsTheWayOutOfARefusal() async throws {
        let (first, root) = try makeStorage(release: releaseIdentity("20261102-los24-r5", build: 200))
        defer { try? FileManager.default.removeItem(at: root) }
        let guest = try await first.create(Guest.fresh(on: host))
        try await first.prepareStart(guest)
        try Data("android data".utf8).write(to: first.locations.data)
        try first.completeRun()

        let older = TwoDiskStorage(
            locations: first.locations, release: releaseIdentity("20261031-los24-r4", build: 100),
            releaseDirectory: first.releaseDirectory
        )
        #expect(older.startPlan() == .blocked(.olderAndroid))
        await #expect(throws: InstanceError.self) { try await older.prepareStart(guest) }
        try await older.erase()
        #expect(older.startPlan() == .firstStart)
        let again = try await older.create(guest)
        try await older.prepareStart(again)
    }

    @Test func theGuestIsExcludedFromBackupsUnlessTheUserAsks() async throws {
        let (storage, root) = try makeStorage()
        defer { try? FileManager.default.removeItem(at: root) }
        var guest = try await storage.create(Guest.fresh(on: host))
        try Data(count: 4096).write(to: storage.locations.data)

        // A URL caches the resource values it has read, so each read starts
        // from the path. macOS applies a cleared flag through the backup
        // service, so it reads back a moment after the call returns; the
        // wait is bounded.
        func excluded(_ url: URL) throws -> Bool? {
            try URL(fileURLWithPath: url.path).resourceValues(forKeys: [.isExcludedFromBackupKey]).isExcludedFromBackup
        }
        func settles(_ url: URL, to value: Bool) throws -> Bool {
            let deadline = Date().addingTimeInterval(5)
            while try excluded(url) != value {
                if Date() > deadline { return false }
                Thread.sleep(forTimeInterval: 0.02)
            }
            return true
        }
        try storage.save(guest)
        let paths = [storage.locations.directory, storage.locations.data, storage.locations.settings, storage.locations.instance]
        for url in paths {
            #expect(try settles(url, to: true), "\(url.lastPathComponent)")
        }

        guest.includeInBackups = true
        try storage.save(guest)
        for url in paths {
            #expect(try settles(url, to: false), "\(url.lastPathComponent)")
        }
    }
}

struct DiskPathTests {
    @Test func theRuntimesKernelArgumentRefusesWhatItCannotCarry() {
        #expect(DiskPath.isUsable("/var/mobile/Containers/Data/Application/0E1F-2A/Library/Guest/data.img"))
        #expect(!DiskPath.isUsable(""))
        // The Application Support directory has a space in its name, which is
        // why a guest does not live there.
        #expect(!DiskPath.isUsable("/Library/Application Support/Guest/data.img"))
        for refused in ["/a\tb", "/a\nb", "/a,b", "/a:b", "/a\"b", "/caf\u{e0}/data.img"] {
            #expect(!DiskPath.isUsable(refused), "\(refused.debugDescription)")
        }
    }

    @Test func aGuestIsNotWrittenToAPathTheRuntimeCannotBoot() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-core-tests-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        let directory = root.appendingPathComponent("Application Support").appendingPathComponent("Guest")
        let storage = TwoDiskStorage(
            locations: GuestLocations(directory: directory), release: releaseIdentity(),
            releaseDirectory: root.appendingPathComponent("andarium-system")
        )
        let guest = Guest.fresh(on: host)
        let expected = StorageError.unusableDiskPath(directory.appendingPathComponent("data.img").path)

        await #expect(throws: expected) { try await storage.create(guest) }
        await #expect(throws: expected) { try await storage.erase() }
        await #expect(throws: expected) { try await storage.prepareStart(guest) }
        #expect(!FileManager.default.fileExists(atPath: directory.path))
        #expect(expected.errorDescription?.contains("Application Support") == true)
    }

    @Test func theReleaseDirectoryNeedsAUsablePathToo() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-core-tests-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        let release = root.appendingPathComponent("Andarium 2.app/andarium-system")
        let storage = TwoDiskStorage(
            locations: GuestLocations(directory: root.appendingPathComponent("Guest")), release: releaseIdentity(),
            releaseDirectory: release
        )
        await #expect(throws: StorageError.unusableDiskPath(release.path)) { try await storage.create(Guest.fresh(on: host)) }
    }
}
