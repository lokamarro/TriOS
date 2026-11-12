// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// A real guest paused for want of space, through the production Home, boot
/// and session views. Runs alone with ANDARIUM_SPACE_PAUSE_TESTS=1 and
/// ANDARIUM_GUEST_INSTANCE=space-pause, without ANDARIUM_START. The device's
/// free space is a number the test sets (`makeModel(freeSpace:)`), so no disk
/// is filled. It deletes only that isolated test guest.
///
/// The run is paused when the number falls below the threshold, and what
/// proves it is the guest's own CPU time standing still, with no image of
/// its RAM written to the temporary directory; Resume refuses
/// while the number is short and wakes the guest once it is not, which
/// answers adb again and whose CPU time runs on; Shut down wakes it and
/// ends the run at Home.
@MainActor
struct SpacePauseGuestTests {
    /// The device's free space as the test states it.
    private final class FreeSpace: @unchecked Sendable {
        private let lock = NSLock()
        private var bytes: UInt64
        init(_ bytes: UInt64) { self.bytes = bytes }
        var value: UInt64 {
            get { lock.withLock { bytes } }
            set { lock.withLock { bytes = newValue } }
        }
    }

    private let mib = Limits.bytesPerMiB

    @Test(.timeLimit(.minutes(25)))
    func aGuestIsPausedForSpaceAndResumedOrShutDownByThePerson() async throws {
        guard ProcessInfo.processInfo.environment["ANDARIUM_SPACE_PAUSE_TESTS"] == "1" else { return }
        #expect(ProcessInfo.processInfo.environment["ANDARIUM_START"] == nil)
        try #require(ProcessInfo.processInfo.environment["ANDARIUM_GUEST_INSTANCE"] == "space-pause")
        let directory = AppEntry.guestDirectory(instance: "space-pause")
        if FileManager.default.fileExists(atPath: directory.path) {
            try FileManager.default.removeItem(at: directory)
        }
        defer { try? FileManager.default.removeItem(at: directory) }
        // Just above the pause: the space is read every few seconds.
        let free = FreeSpace(RunSpace.resumeAtBytes + 100 * mib)
        let entry = AppEntry.fromArguments()
        let model = entry.makeModel(freeSpace: { free.value })
        try #require(model.alert == nil, "\(String(describing: model.alert))")
        let lease = try await HostWindow.take()
        defer { lease.give() }
        lease.window.rootViewController = UIHostingController(rootView: RootView(entry: entry).environment(model))
        model.create(Guest(cores: 2, memoryMiB: 1536, capacityGiB: 8))
        let createDeadline = ContinuousClock.now + .seconds(30)
        while model.busy != nil && ContinuousClock.now < createDeadline {
            try await Task.sleep(for: .milliseconds(100))
        }
        try #require(model.guest != nil && model.busy == nil && model.alert == nil)
        let driver = try #require(model.driver)

        let identity = try entry.adbKeyStore.loadOrCreate()
        let operations = AdbGuestOperations(identity: identity, clock: GuestRunClock(), device: "device")
        model.start(.normal)
        try await bootCompleted(model, operations)
        print("SPACE_PAUSE: Android booted; free space \(Formats.size(bytes: free.value))")
        #expect(model.spacePause == nil)

        // Work for the guest to do, so its CPU time visibly runs: a shell that
        // loops until the guest goes (a background job would hold adb's
        // streams open).
        let load = Task {
            _ = try? await operations.shell("while :; do :; done", limit: .seconds(900))
        }
        defer { load.cancel() }
        try await Task.sleep(for: .seconds(3))
        let running = try await cpuAdvance(driver, over: .seconds(3))
        print("SPACE_PAUSE: guest CPU while running: \(running / 1_000_000) ms in 3 s")
        try #require(running > 200_000_000, "the guest's CPU time does not run: \(running) ns")

        // The device runs short: the guest pauses at the next reading, and the
        // screen offers Resume once the suspension is confirmed.
        let temporaryBefore = Self.temporaryBytes()
        free.value = 900 * mib
        try #require(await eventually(.seconds(90)) { model.spacePause != nil }, "no pause: \(model.runReport)")
        try #require(await eventually(.seconds(60)) { model.spacePause?.stage == .paused },
            "the pause was not confirmed: \(String(describing: model.spacePause))")
        #expect(model.spacePause == SpacePause(freeBytes: 900 * mib))
        // The device is short of space: no image of the guest's RAM went to it.
        let temporaryAfter = Self.temporaryBytes()
        let written = temporaryAfter > temporaryBefore ? temporaryAfter - temporaryBefore : 0
        print("SPACE_PAUSE: the temporary directory grew by \(written / 1_000_000) MB")
        #expect(written < 64 * mib, "the pause wrote \(written) bytes to the temporary directory")
        #expect(model.phase == .running(.normal))
        try await expectStopped(driver, "paused")

        // Resume while the space is still short: refused, with the figure now.
        free.value = 1_200 * mib
        model.resumeAfterSpacePause()
        try #require(await eventually(.seconds(30)) { model.spacePause?.stillShort == true })
        #expect(model.spacePause == SpacePause(freeBytes: 1_200 * mib, stillShort: true))
        try await expectStopped(driver, "still paused after a refused Resume")

        // Room again: Resume wakes the guest, which answers and runs on.
        free.value = 3 * Limits.bytesPerGiB
        model.resumeAfterSpacePause()
        try #require(await eventually(.seconds(60)) { model.spacePause == nil })
        let answer = try await operations.shell("getprop sys.boot_completed", limit: .seconds(30))
        #expect(answer.text.hasPrefix("1"), "the resumed guest answered \(answer.text)")
        let again = try await cpuAdvance(driver, over: .seconds(3))
        print("SPACE_PAUSE: guest CPU after Resume: \(again / 1_000_000) ms in 3 s")
        #expect(again > 100_000_000, "the resumed guest's CPU time does not run: \(again) ns")
        #expect(model.phase == .running(.normal))

        // Short again: the watch was armed again, and Shut down ends the run.
        free.value = 800 * mib
        try #require(await eventually(.seconds(90)) { model.spacePause != nil })
        try await expectStopped(driver, "paused the second time")
        model.shutDownAfterSpacePause()
        #expect(model.spacePause?.working == .shuttingDown)
        try #require(await eventually(.seconds(180)) { model.phase == .home }, "no shutdown: \(model.runReport)")
        #expect(model.runEnd.poweredOff)
        #expect(model.spacePause == nil)
        print("SPACE_PAUSE: Shut down ended the run at Home")
    }

    private func bootCompleted(_ model: AppModel, _ operations: AdbGuestOperations) async throws {
        let deadline = ContinuousClock.now + .seconds(5 * 60)
        while ContinuousClock.now < deadline {
            if case .failed = model.phase { break }
            if let result = try? await operations.shell("getprop sys.boot_completed", limit: .seconds(10)),
                result.text.hasPrefix("1"), model.phase == .running(.normal) {
                return
            }
            try await Task.sleep(for: .seconds(2))
        }
        Issue.record("Android did not boot: \(model.runReport)")
        throw CancellationError()
    }

    /// Waits, up to `limit`, for `condition` on the main actor.
    private func eventually(_ limit: Duration, _ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + limit
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(200))
        }
        return condition()
    }

    /// What the regular files of the temporary directory take now.
    private static func temporaryBytes() -> UInt64 {
        let root = URL(fileURLWithPath: NSTemporaryDirectory())
        let keys: [URLResourceKey] = [.isRegularFileKey, .fileAllocatedSizeKey]
        guard let files = FileManager.default.enumerator(at: root, includingPropertiesForKeys: keys) else { return 0 }
        var total: UInt64 = 0
        for case let url as URL in files {
            guard let values = try? url.resourceValues(forKeys: Set(keys)), values.isRegularFile == true else { continue }
            total += UInt64(values.fileAllocatedSize ?? 0)
        }
        return total
    }

    /// The CPU time the guest's vCPU threads used over `interval`.
    private func cpuAdvance(_ driver: any RuntimeDriver, over interval: Duration) async throws -> UInt64 {
        let first = try #require(await driver.readStatistics())
        try await Task.sleep(for: interval)
        let second = try #require(await driver.readStatistics())
        try #require(first.bootIndex == second.bootIndex)
        return second.processorTimeNanoseconds &- first.processorTimeNanoseconds
    }

    /// The guest's CPU time stands still: the pause takes effect in the
    /// background after the screen shows, so it is given a while to settle
    /// and must then hold across a further window.
    private func expectStopped(_ driver: any RuntimeDriver, _ what: String) async throws {
        var settled = false
        let deadline = ContinuousClock.now + .seconds(60)
        while ContinuousClock.now < deadline {
            if try await cpuAdvance(driver, over: .seconds(2)) == 0 {
                settled = true
                break
            }
        }
        try #require(settled, "the guest's CPU time still runs while \(what)")
        let held = try await cpuAdvance(driver, over: .seconds(5))
        #expect(held == 0, "the guest's CPU time ran \(held) ns while \(what)")
        print("SPACE_PAUSE: the guest's CPU time stands still while \(what)")
    }
}
