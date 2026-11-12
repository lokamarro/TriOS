// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

private func identity(
    _ id: String, build: Int, sha: String = String(repeating: "a", count: 64), layout: Int = 1
) -> ReleaseIdentity {
    ReleaseIdentity(
        releaseID: id, buildFingerprint: "fp-\(id)", buildNumber: build, securityPatch: "2026-10-01",
        systemDiskSHA256: sha, dataLayout: layout, minimumGiB: 4, defaultGiB: 8, maximumGiB: 64
    )
}

private let r1 = identity("r1", build: 100)
private let r2 = identity("r2", build: 200)
private let r3 = identity("r3", build: 300)
private let host = HostDevice(
    name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
)

private func makeDirectory() throws -> URL {
    let root = FileManager.default.temporaryDirectory
        .appendingPathComponent("andarium-plan-tests-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    return root
}

struct StartPlanTests {
    /// An instance created with r1, started and completed, then updated to r2
    /// whose start did not complete.
    private func unfinishedUpdate(in directory: URL) throws -> InstanceFiles {
        let files = InstanceFiles(directory: directory)
        try files.apply(InstanceDecision(.create, "new"), release: r1)
        try Data("android data".utf8).write(to: files.data)
        try files.complete(release: r1)
        try files.apply(files.decide(for: r2), release: r2)
        return files
    }

    @Test func eachDecisionHasAPlan() throws {
        let directory = try makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let files = InstanceFiles(directory: directory)
        #expect(files.startPlan(for: r1) == .firstStart)

        try files.apply(InstanceDecision(.create, "new"), release: r1)
        #expect(files.startPlan(for: r1) == .firstStart)
        try Data("android data".utf8).write(to: files.data)
        #expect(files.startPlan(for: r1) == .normal)
        #expect(files.startPlan(for: r2) == .update(from: "r1", to: "r2"))

        try files.complete(release: r1)
        // An older Android does not go back; a newer data layout has no
        // migration; one release with two system disks is a mismatch.
        try files.apply(files.decide(for: r2), release: r2)
        try files.complete(release: r2)
        #expect(files.startPlan(for: r1) == .blocked(.olderAndroid))
        #expect(files.startPlan(for: identity("r3", build: 300, layout: 2)) == .blocked(.newerDataLayout))
        #expect(files.startPlan(for: identity("r2", build: 200, sha: String(repeating: "b", count: 64))) == .blocked(.mismatch))
    }

    @Test func dataNothingRecordedIsAMismatch() throws {
        let directory = try makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let files = InstanceFiles(directory: directory)
        try Data("android data".utf8).write(to: files.data)
        #expect(files.startPlan(for: r1) == .blocked(.mismatch))
    }

    @Test func anUnreadableRecordHasNoPlan() throws {
        let directory = try makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let files = InstanceFiles(directory: directory)
        try Data(#"{"schema": 0}"#.utf8).write(to: files.state)
        #expect(files.startPlan(for: r1) == nil)
    }

    @Test func anUnfinishedUpdateOffersTheWaysThatCanWork() throws {
        let directory = try makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let files = try unfinishedUpdate(in: directory)
        #expect(files.startPlan(for: r2) == .unfinishedUpdate(to: "r2", canRetry: true, canRestore: true))
        // A newer build restores the data and updates directly, without
        // needing the intermediate release to finish first.
        #expect(files.startPlan(for: r3) == .update(from: "r1", to: "r3"))
        // Without its snapshot there is nothing to put back.
        try FileManager.default.removeItem(at: files.snapshot(for: "r2"))
        #expect(files.startPlan(for: r2) == .unfinishedUpdate(to: "r2", canRetry: true, canRestore: false))
        #expect(files.startPlan(for: r3) == .blocked(.mismatch))
    }

    @Test func theBootKindFollowsThePlanAndTheMode() {
        #expect(StartPlan.firstStart.bootKind(.normal) == .firstStart)
        #expect(StartPlan.normal.bootKind(.normal) == .normal)
        #expect(StartPlan.update(from: "r1", to: "r2").bootKind(.normal) == .upgrade)
        #expect(StartPlan.unfinishedUpdate(to: "r2", canRetry: true, canRestore: true).bootKind(.normal) == .upgrade)
        #expect(StartPlan.update(from: "r1", to: "r2").bootKind(.recovery) == .recovery)
        #expect(StartPlan.firstStart.bootKind(.recovery) == .recovery)
    }
}

struct StartVerdictTests {
    private let unfinished = StartPlan.unfinishedUpdate(to: "r2", canRetry: true, canRestore: false)

    @Test func theVerdictFollowsThePlan() {
        let life = Lifecycle(guest: Guest.fresh(on: host))
        #expect(life.startVerdict(.normal, host: host, plan: .firstStart) == .allowed(.firstStart))
        #expect(life.startVerdict(.normal, host: host, plan: .normal) == .allowed(.normal))
        #expect(life.startVerdict(.normal, host: host, plan: .update(from: "r1", to: "r2")) == .allowed(.upgrade))
        #expect(life.startVerdict(.recovery, host: host, plan: .update(from: "r1", to: "r2")) == .allowed(.recovery))
        #expect(life.startVerdict(.normal, host: host, plan: .blocked(.olderAndroid)) == .blocked(.olderAndroid))
        #expect(life.startVerdict(.recovery, host: host, plan: .blocked(.mismatch)) == .blocked(.mismatch))
    }

    @Test func startTriesAnUnfinishedUpdateAgain() {
        let life = Lifecycle(guest: Guest.fresh(on: host))
        #expect(life.startVerdict(.normal, host: host, plan: unfinished) == .allowed(.upgrade))
        // Recovery does not finish the update, and another Android cannot
        // try it again: Restore or Erase is the way on.
        #expect(life.startVerdict(.recovery, host: host, plan: unfinished) == .unfinishedUpdate)
        let other = StartPlan.unfinishedUpdate(to: "r2", canRetry: false, canRestore: true)
        #expect(life.startVerdict(.normal, host: host, plan: other) == .unfinishedUpdate)
        #expect(!other.allowsStart && unfinished.allowsStart)
        #expect(!StartPlan.blocked(.mismatch).allowsStart)
        #expect(StartPlan.update(from: "r1", to: "r2").allowsStart && StartPlan.firstStart.allowsStart)
    }

    @Test func theScreensOfferNoStartThePlanRefuses() {
        let other = StartPlan.unfinishedUpdate(to: "r2", canRetry: false, canRestore: true)
        for mode in [StartMode.normal, .recovery] {
            #expect(!StartPlan.blocked(.mismatch).allowsStart(mode))
            #expect(!other.allowsStart(mode))
            #expect(StartPlan.firstStart.allowsStart(mode) && StartPlan.normal.allowsStart(mode))
            #expect(StartPlan.update(from: "r1", to: "r2").allowsStart(mode))
        }
        // Recovery does not finish the update, so it waits like the rest.
        #expect(unfinished.allowsStart(.normal))
        #expect(!unfinished.allowsStart(.recovery))
        // The lifecycle's verdict agrees with it for every plan.
        let life = Lifecycle(guest: Guest.fresh(on: host))
        for plan in [unfinished, other, StartPlan.blocked(.olderAndroid), .normal, .firstStart] {
            for mode in [StartMode.normal, .recovery] {
                let allowed: Bool
                if case .allowed = life.startVerdict(mode, host: host, plan: plan) { allowed = true } else { allowed = false }
                #expect(allowed == plan.allowsStart(mode))
            }
        }
    }

    /// Home shows Start in recovery from the first start on and disables it
    /// before: the guest has nothing for Recovery to work on until then.
    @Test func homeOffersRecoveryOnlyAfterTheFirstStart() {
        #expect(!StartPlan.firstStart.offersRecovery)
        #expect(StartPlan.normal.offersRecovery)
        #expect(StartPlan.update(from: "r1", to: "r2").offersRecovery)
        // What an update that did not finish, or a guest that cannot start,
        // refuses stays refused.
        #expect(!unfinished.offersRecovery)
        #expect(!StartPlan.blocked(.mismatch).offersRecovery)
        // Only what Home offers waits: the lifecycle still allows the start,
        // which an automation launch asks for on a guest it has just made.
        let life = Lifecycle(guest: Guest.fresh(on: host))
        #expect(life.startVerdict(.recovery, host: host, plan: .firstStart) == .allowed(.recovery))
        #expect(StartPlan.firstStart.allowsStart(.recovery))
    }

    @Test func aBlockedStartLeavesTheLifeAtHome() {
        var life = Lifecycle(guest: Guest.fresh(on: host))
        #expect(life.start(.normal, host: host, plan: .blocked(.olderAndroid)) == .blocked(.olderAndroid))
        #expect(life.phase == .home)
        #expect(life.start(.recovery, host: host, plan: unfinished) == .unfinishedUpdate)
        #expect(life.phase == .home)
        #expect(life.start(.normal, host: host, plan: unfinished) == .allowed(.upgrade))
        #expect(life.phase == .booting(.upgrade))
    }
}
