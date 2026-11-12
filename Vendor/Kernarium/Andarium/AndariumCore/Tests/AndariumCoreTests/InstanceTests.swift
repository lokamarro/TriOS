// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

private func release(
    _ id: String = "rel-b", build: Int = 200, layout: Int = 1, sha: String = String(repeating: "b", count: 64)
) -> ReleaseIdentity {
    ReleaseIdentity(
        releaseID: id, buildFingerprint: "fp-\(id)", buildNumber: build, securityPatch: "2026-10-01",
        systemDiskSHA256: sha, dataLayout: layout, minimumGiB: 4, defaultGiB: 8, maximumGiB: 64
    )
}

private func temporaryDirectory() throws -> URL {
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("andarium-instance-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    return url
}

/// The vectors every implementation of the update rules reproduces
/// (`android/lineage-24/support/instance/update-decisions.json`).
struct DecisionVectorTests {
    private struct Vector: Decodable {
        struct Instance: Decodable {
            var releaseID: String
            var buildNumber: Int
            var systemDiskSHA256: String
            var dataLayout: Int
            var pendingToReleaseID: String?
            enum CodingKeys: String, CodingKey {
                case releaseID = "release_id", buildNumber = "build_number"
                case systemDiskSHA256 = "system_disk_sha256", dataLayout = "data_layout"
                case pendingToReleaseID = "pending_to_release_id"
            }
        }
        struct Release: Decodable {
            var releaseID: String
            var buildNumber: Int
            var systemDiskSHA256: String
            var dataLayout: Int
            enum CodingKeys: String, CodingKey {
                case releaseID = "release_id", buildNumber = "build_number"
                case systemDiskSHA256 = "system_disk_sha256", dataLayout = "data_layout"
            }
        }
        var name: String
        var instance: Instance?
        var dataDisk: Bool
        var snapshot: Bool
        var release: Release
        var flags: [String]
        var expect: String
        enum CodingKeys: String, CodingKey {
            case name, instance, release, flags, expect, snapshot
            case dataDisk = "data_disk"
        }
    }

    /// What a start's `sys.boot_completed` records on the instance.
    private struct Transition: Decodable {
        struct Expectation: Decodable {
            var pendingToReleaseID: String?
            var snapshot: Bool
            enum CodingKeys: String, CodingKey {
                case pendingToReleaseID = "pending_to_release_id", snapshot
            }
        }
        var name: String
        var event: String
        var instance: Vector.Instance?
        var snapshot: Bool
        var release: Vector.Release
        var expect: Expectation?
        var refused: Bool
        enum CodingKeys: String, CodingKey { case name, event, instance, snapshot, release, expect }

        init(from decoder: Decoder) throws {
            let values = try decoder.container(keyedBy: CodingKeys.self)
            name = try values.decode(String.self, forKey: .name)
            event = try values.decode(String.self, forKey: .event)
            instance = try values.decodeIfPresent(Vector.Instance.self, forKey: .instance)
            snapshot = try values.decode(Bool.self, forKey: .snapshot)
            release = try values.decode(Vector.Release.self, forKey: .release)
            // The expectation is the word "refused" or the state afterwards.
            if (try? values.decode(String.self, forKey: .expect)) == "refused" {
                refused = true
                expect = nil
            } else {
                refused = false
                expect = try values.decode(Expectation.self, forKey: .expect)
            }
        }
    }

    private struct Document: Decodable {
        var cases: [Vector]
        var transitions: [Transition]
    }

    private static func document() throws -> Document {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { url.deleteLastPathComponent() }
        url.append(path: "android/lineage-24/support/instance/update-decisions.json")
        return try JSONDecoder().decode(Document.self, from: Data(contentsOf: url))
    }

    private static func vectors() throws -> [Vector] { try document().cases }

    @Test func everyTransitionIsReproduced() throws {
        let transitions = try Self.document().transitions
        #expect(transitions.count == 5)
        for transition in transitions {
            let root = try temporaryDirectory()
            defer { try? FileManager.default.removeItem(at: root) }
            let files = InstanceFiles(directory: root.appendingPathComponent("Guest"))
            if let instance = transition.instance {
                var state = InstanceState(
                    dataLayout: instance.dataLayout, releaseID: instance.releaseID, buildFingerprint: "fp",
                    buildIncremental: String(instance.buildNumber), systemDiskSHA256: instance.systemDiskSHA256,
                    createdWith: "rel-0", lastBooted: "2026-10-02T00:00:00Z"
                )
                if let target = instance.pendingToReleaseID {
                    state.pendingUpdate = InstanceState.Pending(
                        toReleaseID: target, snapshot: "data.img.pre-\(target)",
                        from: InstanceState.Origin(
                            releaseID: "rel-0", buildFingerprint: "fp", buildIncremental: "50",
                            systemDiskSHA256: String(repeating: "0", count: 64), dataLayout: 1,
                            lastBooted: "2026-10-01T00:00:00Z"
                        )
                    )
                }
                try files.write(state)
                try Data("guest data".utf8).write(to: files.data)
            }
            if transition.snapshot {
                try FileManager.default.createDirectory(at: files.directory, withIntermediateDirectories: true)
                try Data("snapshot".utf8).write(to: files.snapshot(for: "rel-b"))
            }
            let target = release(
                transition.release.releaseID, build: transition.release.buildNumber,
                layout: transition.release.dataLayout, sha: transition.release.systemDiskSHA256
            )
            if transition.refused {
                #expect(throws: InstanceError.self, "\(transition.name)") { try files.booted(release: target) }
                continue
            }
            if transition.event == "boot_completed" { try files.booted(release: target) }
            let expected = try #require(transition.expect)
            let after = try #require(try files.readState())
            #expect(after.pendingUpdate?.toReleaseID == expected.pendingToReleaseID, "\(transition.name)")
            #expect(
                FileManager.default.fileExists(atPath: files.snapshot(for: "rel-b").path) == expected.snapshot,
                "\(transition.name)"
            )
            #expect(try Data(contentsOf: files.data) == Data("guest data".utf8), "\(transition.name)")
        }
    }

    @Test func everyVectorIsReproduced() throws {
        let cases = try Self.vectors()
        #expect(cases.count == 28)
        for vector in cases {
            let state = vector.instance.map { instance in
                var state = InstanceState(
                    dataLayout: instance.dataLayout, releaseID: instance.releaseID, buildFingerprint: "fp",
                    buildIncremental: String(instance.buildNumber), systemDiskSHA256: instance.systemDiskSHA256,
                    createdWith: instance.releaseID, lastBooted: "2026-10-01T00:00:00Z"
                )
                if let target = instance.pendingToReleaseID {
                    state.pendingUpdate = InstanceState.Pending(
                        toReleaseID: target, snapshot: "data.img.pre-\(target)",
                        from: InstanceState.Origin(
                            releaseID: "rel-a", buildFingerprint: "fp", buildIncremental: "100",
                            systemDiskSHA256: String(repeating: "a", count: 64), dataLayout: instance.dataLayout,
                            lastBooted: "2026-09-01T00:00:00Z"
                        )
                    )
                }
                return state
            }
            let flags = InstanceFlags(
                retry: vector.flags.contains("retry"), restoreSnapshot: vector.flags.contains("restore_snapshot"),
                reset: vector.flags.contains("reset")
            )
            let target = release(
                vector.release.releaseID, build: vector.release.buildNumber, layout: vector.release.dataLayout,
                sha: vector.release.systemDiskSHA256
            )
            let decision = InstanceRules.decide(
                state: state, dataDiskExists: vector.dataDisk, snapshotExists: vector.snapshot, release: target, flags: flags
            )
            #expect(decision.action.rawValue == vector.expect, "\(vector.name)")
        }
    }
}

struct InstanceFileTests {
    private func started() throws -> (InstanceFiles, URL, ReleaseIdentity) {
        let root = try temporaryDirectory()
        let files = InstanceFiles(directory: root.appendingPathComponent("Guest"))
        let first = release("rel-a", build: 100, sha: String(repeating: "a", count: 64))
        let decision = try files.decide(for: first)
        #expect(decision.action == .create)
        try files.apply(decision, release: first)
        try Data("android data".utf8).write(to: files.data)
        return (files, root, first)
    }

    @Test func aNewInstanceRecordsItsReleaseAndAnOldOneBootsAgain() throws {
        let (files, root, first) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let state = try #require(try files.readState())
        #expect(state.releaseID == "rel-a" && state.buildNumber == 100 && state.createdWith == "rel-a")
        #expect(state.lastCompleted == nil && state.pendingUpdate == nil)
        let decision = try files.decide(for: first)
        #expect(decision.action == .boot)
        try files.apply(decision, release: first)
    }

    @Test func theStateIsWrittenWithTheSchemasKeys() throws {
        let (files, root, _) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let object = try #require(try JSONSerialization.jsonObject(with: Data(contentsOf: files.state)) as? [String: Any])
        #expect(Set(object.keys) == InstanceState.keys)
        #expect(object["schema"] as? Int == 1)
        #expect(object["last_completed"] is NSNull && object["pending_update"] is NSNull)
    }

    @Test func anUpdateSnapshotsTheDataAndCompletingItDeletesTheSnapshot() throws {
        let (files, root, _) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let second = release("rel-b", build: 200)
        let decision = try files.decide(for: second)
        #expect(decision.action == .update)
        let updated = try files.apply(decision, release: second)
        let snapshot = files.snapshot(for: "rel-b")
        #expect(FileManager.default.fileExists(atPath: snapshot.path))
        #expect(try Data(contentsOf: snapshot) == Data("android data".utf8))
        #expect(updated.pendingUpdate?.toReleaseID == "rel-b" && updated.pendingUpdate?.from.releaseID == "rel-a")
        // Until the update completes, starting is refused.
        #expect(try files.decide(for: second).action == .refusePending)

        let done = try files.complete(release: second)
        #expect(done.pendingUpdate == nil && done.lastCompleted?.releaseID == "rel-b")
        #expect(!FileManager.default.fileExists(atPath: snapshot.path))
        #expect(try files.decide(for: second).action == .boot)
    }

    /// The sequence that lost a week of user data: r13 settles, r15 updates and
    /// boots, the host is killed before Android powers off, r15 starts again,
    /// and r16 arrives.
    @Test func anUpdateThatBootedIsNotRolledBackByTheNextRelease() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let files = InstanceFiles(directory: root.appendingPathComponent("Guest"))
        let r13 = release("r13", build: 13, sha: String(repeating: "a", count: 64))
        let r15 = release("r15", build: 15, sha: String(repeating: "b", count: 64))
        let r16 = release("r16", build: 16, sha: String(repeating: "c", count: 64))
        try files.apply(try files.decide(for: r13), release: r13)
        try Data("data written on r13".utf8).write(to: files.data)
        try files.complete(release: r13)

        let update = try files.decide(for: r15)
        #expect(update.action == .update)
        try files.apply(update, release: r15)
        #expect(FileManager.default.fileExists(atPath: files.snapshot(for: "r15").path))
        // r15 reaches boot_completed: the update is applied.
        let booted = try files.booted(release: r15)
        #expect(booted.pendingUpdate == nil)
        #expect(!FileManager.default.fileExists(atPath: files.snapshot(for: "r15").path))
        try Data("r15 upgraded data and a week of user data".utf8).write(to: files.data)
        // The host dies without a clean power-off; the next start is an
        // ordinary one, not a retry.
        let next = try files.decide(for: r15)
        #expect(next.action == .boot)
        try files.apply(next, release: r15)
        try Data("r15 upgraded data and a month of user data".utf8).write(to: files.data)

        let newer = try files.decide(for: r16)
        #expect(newer.action == .update)
        let updated = try files.apply(newer, release: r16)
        let month = Data("r15 upgraded data and a month of user data".utf8)
        #expect(try Data(contentsOf: files.data) == month)
        #expect(try Data(contentsOf: files.snapshot(for: "r16")) == month)
        #expect(updated.pendingUpdate?.from.releaseID == "r15" && updated.pendingUpdate?.toReleaseID == "r16")
        // The update to r16 is now the pending one, with a snapshot of its own.
        #expect(try files.startPlan(for: r16) == .unfinishedUpdate(to: "r16", canRetry: true, canRestore: true))
    }

    @Test func aFailedUpdateCanBeRetriedOrRestored() throws {
        let (files, root, _) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let second = release("rel-b", build: 200)
        try files.apply(try files.decide(for: second), release: second)
        try Data("data written by the update".utf8).write(to: files.data)

        let retry = try files.decide(for: second, flags: InstanceFlags(retry: true))
        #expect(retry.action == .retry)
        try files.apply(retry, release: second)
        #expect(try files.readState()?.pendingUpdate != nil)

        let restore = try files.decide(for: second, flags: InstanceFlags(restoreSnapshot: true))
        #expect(restore.action == .restore)
        // The data goes back and nothing else happens.
        let restored = try files.apply(restore, release: second)
        #expect(try Data(contentsOf: files.data) == Data("android data".utf8))
        #expect(restored.releaseID == "rel-a" && restored.pendingUpdate == nil)
        // A normal start then decides afresh: the update again, which is what
        // a restore that went straight on to update would have reached.
        let next = try files.decide(for: second)
        #expect(next.action == .update)
        let again = try files.apply(next, release: second)
        #expect(try Data(contentsOf: files.data) == Data("android data".utf8))
        #expect(again.pendingUpdate?.toReleaseID == "rel-b")
    }

    @Test func aNewerReleaseRestoresAnUnfinishedUpdateBeforeStarting() throws {
        let (files, root, first) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let second = release("rel-b", build: 200)
        let newer = release("rel-d", build: 10000, sha: String(repeating: "d", count: 64))
        try files.apply(try files.decide(for: second), release: second)
        try Data("partially upgraded data".utf8).write(to: files.data)

        let decision = try files.decide(for: newer)
        #expect(decision.action == .update)
        let updated = try files.apply(decision, release: newer)
        #expect(try Data(contentsOf: files.data) == Data("android data".utf8))
        #expect(try Data(contentsOf: files.snapshot(for: "rel-d")) == Data("android data".utf8))
        #expect(!FileManager.default.fileExists(atPath: files.snapshot(for: "rel-b").path))
        #expect(updated.releaseID == "rel-d" && updated.pendingUpdate?.from.releaseID == first.releaseID)
        #expect(updated.pendingUpdate?.toReleaseID == "rel-d" && updated.createdWith == first.releaseID)
        try files.complete(release: newer)
        #expect(!FileManager.default.fileExists(atPath: files.snapshot(for: "rel-d").path))
    }

    @Test func aMissingSnapshotLeavesTheUnfinishedUpdatesDataAlone() throws {
        let (files, root, _) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let second = release("rel-b", build: 200)
        try files.apply(try files.decide(for: second), release: second)
        try Data("partially upgraded data".utf8).write(to: files.data)
        try FileManager.default.removeItem(at: files.snapshot(for: "rel-b"))
        let state = try Data(contentsOf: files.state)
        let newer = release("rel-d", build: 10000, sha: String(repeating: "d", count: 64))
        let decision = try files.decide(for: newer)
        #expect(decision.action == .refuseConflict)
        #expect(throws: InstanceError.self) { try files.apply(decision, release: newer) }
        #expect(try Data(contentsOf: files.state) == state)
        #expect(try Data(contentsOf: files.data) == Data("partially upgraded data".utf8))
    }

    @Test func aRefusalChangesNothing() throws {
        let (files, root, _) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let older = release("rel-0", build: 50)
        let decision = try files.decide(for: older)
        #expect(decision.action == .refuseDowngrade)
        let before = try Data(contentsOf: files.state)
        #expect(throws: InstanceError.self) { try files.apply(decision, release: older) }
        #expect(try Data(contentsOf: files.state) == before)
    }

    @Test func resetDeletesTheInstanceAndCreatesItAnew() throws {
        let (files, root, _) = try started()
        defer { try? FileManager.default.removeItem(at: root) }
        let second = release("rel-b", build: 200)
        try files.apply(try files.decide(for: second), release: second)
        let reset = try files.decide(for: second, flags: InstanceFlags(reset: true))
        #expect(reset.action == .reset)
        let created = try files.apply(reset, release: second)
        #expect(created.releaseID == "rel-b" && created.createdWith == "rel-b" && created.pendingUpdate == nil)
        #expect(!FileManager.default.fileExists(atPath: files.data.path))
        #expect(!FileManager.default.fileExists(atPath: files.snapshot(for: "rel-b").path))
    }

    @Test func aReleaseIdentityIsReadFromTheManifestAndTheLayout() throws {
        let manifest = #"{"release_id": "20261102-los24-r5", "android": {"fingerprint": "Andarium/lineage_andarium/andarium:17/a/b:user/release-keys", "build_number": 1793463711, "security_patch": "2026-10-01"}}"#
        let layout = #"{"system_disk": {"sha256": "\#(String(repeating: "c", count: 64))"}, "data_disk": {"layout_version": 1, "capacity_gib": {"minimum": 4, "default": 8, "maximum": 64}}}"#
        let identity = try ReleaseIdentity(manifest: Data(manifest.utf8), layout: Data(layout.utf8))
        #expect(identity.releaseID == "20261102-los24-r5" && identity.buildNumber == 1_793_463_711)
        #expect(identity.dataLayout == 1 && identity.minimumGiB == 4 && identity.defaultGiB == 8 && identity.maximumGiB == 64)
        // A manifest of an earlier schema has no build number.
        let old = #"{"release_id": "20261031-los24-r4", "android": {"fingerprint": "f"}}"#
        #expect(throws: InstanceError.self) { try ReleaseIdentity(manifest: Data(old.utf8), layout: Data(layout.utf8)) }
    }
}
