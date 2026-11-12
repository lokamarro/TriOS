// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Why a guest cannot start with the Android this build carries. Only erasing
/// the guest ends each of these.
public enum StartBlock: Equatable, Sendable {
    /// The Android in this build is older than the one the data was started
    /// with; Android's key store and package manager do not go back.
    case olderAndroid
    /// The data uses a newer layout than this build can migrate.
    case newerDataLayout
    /// The guest's files do not match the Android in this build: another
    /// system disk under the same release, or data nothing recorded.
    case mismatch
}

extension StartBlock {
    public var title: String { "This guest cannot start" }

    /// What Home says; erasing the guest is the way out of each.
    public var explanation: String {
        switch self {
        case .olderAndroid:
            "This app version is older than the one the guest last started with, and its data does not go back. Install a newer version of Andarium, or erase the guest to start again with this one."
        case .newerDataLayout:
            "The guest's data uses a newer layout than this app version can open. Erase the guest to start again with this one."
        case .mismatch:
            "The guest's files do not match this app version. Erase the guest to start again."
        }
    }
}

/// How the guest the user picks up on Home would start, from the instance on
/// disk (`InstanceFiles.startPlan`); nothing is changed to find out.
public enum StartPlan: Equatable, Sendable {
    /// The guest has no data yet: Android sets itself up.
    case firstStart
    /// The Android the data was last started with.
    case normal
    /// A newer Android on the same data layout. An unfinished update's data
    /// is restored first; the data is snapshotted before Android upgrades it.
    case update(from: String, to: String)
    /// An update started and did not reach `sys.boot_completed`; one that did
    /// counts as applied and is not this. Start tries the
    /// update again when this Android is the one that was updating
    /// (`canRetry`); Restore puts back the data the update started from
    /// (`canRestore`, only while that copy exists); erasing the guest starts
    /// over.
    case unfinishedUpdate(to: String, canRetry: Bool, canRestore: Bool)
    /// Nothing but erasing the guest helps.
    case blocked(StartBlock)

    /// Whether the large Start button can start the guest. A blocked guest,
    /// and an update that cannot be tried again with this Android, wait for
    /// Restore or Erase.
    public var allowsStart: Bool {
        switch self {
        case .blocked: false
        case .unfinishedUpdate(_, let canRetry, _): canRetry
        case .firstStart, .normal, .update: true
        }
    }

    /// Whether a start in `mode` can go ahead; the screens offer no start
    /// that cannot, and refuse none with a message. Recovery does not finish
    /// an update, so it waits for Restore or Erase as the rest do.
    public func allowsStart(_ mode: StartMode) -> Bool {
        switch self {
        case .unfinishedUpdate(_, let canRetry, _): mode == .normal && canRetry
        case .blocked, .firstStart, .normal, .update: allowsStart
        }
    }

    /// Whether Home offers Start in recovery. Until the guest has started
    /// once, which is when the runtime creates its data disk (`firstStart`),
    /// Recovery has nothing to work on and the control is disabled, not left
    /// out; erasing the guest and setting up a new one returns to that state.
    /// It decides only what Home offers: a start that arrives another way, as
    /// an automation launch does, is the lifecycle's to allow
    /// (`allowsStart(_:)`).
    public var offersRecovery: Bool {
        if case .firstStart = self { return false }
        return allowsStart(.recovery)
    }

    /// What a start in `mode` is, for the words and steps of the boot.
    public func bootKind(_ mode: StartMode) -> BootKind {
        if mode == .recovery { return .recovery }
        switch self {
        case .firstStart: return .firstStart
        case .update, .unfinishedUpdate: return .upgrade
        case .normal, .blocked: return .normal
        }
    }
}

extension InstanceRules {
    /// What a start would do, in the terms of Home. It follows `decide` and
    /// adds what Home needs to offer the ways out of an unfinished update.
    public static func plan(
        state: InstanceState?, dataDiskExists: Bool, snapshotExists: Bool, release: ReleaseIdentity
    ) -> StartPlan {
        let decision = decide(
            state: state, dataDiskExists: dataDiskExists, snapshotExists: snapshotExists, release: release
        )
        switch decision.action {
        case .create: return .firstStart
        case .boot, .retry, .restore: return .normal
        case .update:
            return .update(from: state?.pendingUpdate?.from.releaseID ?? state?.releaseID ?? "", to: release.releaseID)
        case .migrate: return .blocked(.newerDataLayout)
        case .refuseDowngrade: return .blocked(.olderAndroid)
        case .refuseConflict: return .blocked(.mismatch)
        case .refusePending:
            guard let pending = state?.pendingUpdate else { return .blocked(.mismatch) }
            return .unfinishedUpdate(
                to: pending.toReleaseID,
                canRetry: release.releaseID == pending.toReleaseID && dataDiskExists,
                canRestore: snapshotExists
            )
        case .reset: return .firstStart
        }
    }
}

extension InstanceFiles {
    /// The plan for `release` from the files, or nil when `instance.json`
    /// cannot be read (a guest an earlier layout made): a start then fails
    /// with the reason, as it always did.
    public func startPlan(for release: ReleaseIdentity) -> StartPlan? {
        let recorded: InstanceState?
        do {
            recorded = try readState()
        } catch {
            return nil
        }
        let manager = FileManager.default
        let snapshotExists = recorded?.pendingUpdate.map {
            manager.fileExists(atPath: directory.appendingPathComponent($0.snapshot).path)
        } ?? false
        return InstanceRules.plan(
            state: recorded, dataDiskExists: manager.fileExists(atPath: data.path),
            snapshotExists: snapshotExists, release: release
        )
    }
}
