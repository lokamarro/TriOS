// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import GuestHostCore
import Testing
@testable import Kernarium

/// The low-space pause on a real guest. The application boots its own guest
/// as it launches, and this test plays the device's free space through the
/// space harness and the person's answers through the screen's own calls, so
/// the pause is proven on the runtime without filling a disk. It runs only
/// when the launch asks (`KERNARIUM_SPACE_PAUSE_TESTS=1` with
/// `KERNARIUM_SPACE_HARNESS=1`) and ends the guest, which a launch boots once.
///
/// The guest is paused while it is still booting, so its processor time moves
/// before the pause and has to stand still after it, and what the pause wrote
/// is measured on the device: a hibernation image of the guest's RAM would
/// be gigabytes.
@MainActor
struct SpacePauseGuestTests {
    nonisolated static let enabledKey = "KERNARIUM_SPACE_PAUSE_TESTS"
    nonisolated static var enabled: Bool {
        ProcessInfo.processInfo.environment[enabledKey] == "1"
    }

    /// A launch that starts on a device short of space: the harness's
    /// free-space file, put in Documents before the launch, holds a number
    /// below a start's need.
    nonisolated static let startShortKey = "KERNARIUM_SPACE_START_SHORT"
    nonisolated static var startShort: Bool {
        ProcessInfo.processInfo.environment[startShortKey] == "1"
    }

    private let launcher = EmulatorLauncher.shared
    private let space = GuestSpacePause.shared
    private let effects = SystemSpacePauseEffects(
        persistence: GuestPersistence.shared
    )

    private func say(_ message: String) {
        print("SPACEPROOF: \(message)")
    }

    private func setFreeSpace(mebibytes: Int) throws {
        try String(mebibytes).write(
            to: RootfsImage.documentsDirectory
                .appendingPathComponent(SpaceHarness.freeSpaceFile),
            atomically: true,
            encoding: .utf8
        )
    }

    private func clearFreeSpace() {
        try? FileManager.default.removeItem(
            at: RootfsImage.documentsDirectory
                .appendingPathComponent(SpaceHarness.freeSpaceFile)
        )
    }

    /// The disk space a directory uses, without the rootfs images, which are
    /// the application's inputs and do not change.
    private func allocatedBytes(in root: URL) -> UInt64 {
        var total: UInt64 = 0
        let files = FileManager.default.enumerator(
            at: root,
            includingPropertiesForKeys: [.totalFileAllocatedSizeKey],
            options: [.skipsHiddenFiles]
        )
        while let url = files?.nextObject() as? URL {
            if url.pathExtension == "erofs" { continue }
            total += UInt64(
                (try? url.resourceValues(
                    forKeys: [.totalFileAllocatedSizeKey]
                ))?.totalFileAllocatedSize ?? 0
            )
        }
        return total
    }

    /// The disk space the temporary directory uses, where a RAM image would
    /// go, and the guest's storage in Documents, which the guest itself
    /// writes to.
    private func written() -> (temporary: UInt64, documents: UInt64) {
        (
            allocatedBytes(in: FileManager.default.temporaryDirectory),
            allocatedBytes(in: RootfsImage.documentsDirectory)
        )
    }

    /// The processor time the guest used over one second, nil when the
    /// runtime cannot say.
    private func usedOverASecond() async throws -> UInt64? {
        let before = await effects.guestProcessorTime()
        try await Task.sleep(for: .seconds(1))
        guard let before, let after = await effects.guestProcessorTime() else {
            return nil
        }
        return after >= before ? after - before : nil
    }

    private func usedOverSeconds(_ seconds: Int) async throws -> [UInt64] {
        var used: [UInt64] = []
        for _ in 0..<seconds {
            if let one = try await usedOverASecond() { used.append(one) }
        }
        return used
    }

    private func millis(_ nanoseconds: [UInt64]) -> [UInt64] {
        nanoseconds.map { $0 / 1_000_000 }
    }

    @Test(
        .enabled(if: SpacePauseGuestTests.enabled),
        .timeLimit(.minutes(30))
    )
    func aRealGuestIsPausedHeldResumedAndShutDown() async throws {
        try #require(
            SpaceHarness.current.enabled,
            "KERNARIUM_SPACE_HARNESS=1 is not set"
        )
        defer { clearFreeSpace() }
        try setFreeSpace(mebibytes: 8_192)

        try #require(
            try await eventually(within: .seconds(600), every: .milliseconds(200)) {
                launcher.isActive
            },
            "the application's guest did not run: \(launcher.phase)"
        )
        let guest = launcher.guestName
        say("guest \(guest) is running")

        // The guest is busy while it boots, which is when a pause is proven.
        var busy: UInt64 = 0
        for _ in 0..<180 {
            if let used = try await usedOverASecond(), used >= 100_000_000 {
                busy = used
                break
            }
        }
        try #require(busy > 0, "the guest's processor time never moved")
        say("processor time while running: \(busy / 1_000_000) ms per second")
        let writtenBefore = written()

        // Below the line the guest is suspended, and holds still.
        try setFreeSpace(mebibytes: 700)
        let began = ContinuousClock.now
        try #require(
            try await eventually(within: .seconds(180), every: .milliseconds(100)) {
                space.pause?.stage == .paused
            },
            "the guest was not paused: \(String(describing: space.pause))"
        )
        say("paused after \(ContinuousClock.now - began)")
        let policy = try #require(GuestPersistence.shared.background)
        #expect(policy.spaceHeld)
        #expect(space.pause?.ending == .stop)
        #expect(space.pause?.guest == guest)
        let held = try await usedOverSeconds(5)
        say("processor time while paused (ms per second): \(millis(held))")
        #expect(held.count == 5)
        #expect(held.allSatisfy { $0 <= 5_000_000 })
        let writtenPaused = written()
        let grown = writtenPaused.temporary > writtenBefore.temporary ?
            writtenPaused.temporary - writtenBefore.temporary : 0
        let guestGrown = writtenPaused.documents > writtenBefore.documents ?
            writtenPaused.documents - writtenBefore.documents : 0
        say(
            "temporary directory grew by \(Formats.size(bytes: grown)), " +
                "Documents by \(Formats.size(bytes: guestGrown)) while the pause came"
        )
        #expect(grown < 256 * Bytes.mib)

        // A lifecycle edge does not wake it: the GPU is held and released,
        // and the guest goes on standing still, still paused.
        policy.didEnterBackground()
        let inBackground = try await usedOverSeconds(3)
        policy.willEnterForeground()
        let afterReturn = try await usedOverSeconds(3)
        say("processor time across a background edge: \(millis(inBackground + afterReturn))")
        #expect((inBackground + afterReturn).allSatisfy { $0 <= 5_000_000 })
        #expect(space.pause?.stage == .paused)
        #expect(policy.spaceHeld)

        // Resume refuses while the space is short.
        try setFreeSpace(mebibytes: 1_200)
        space.resume()
        try #require(
            try await eventually(within: .seconds(30)) {
                space.pause?.working == nil && space.pause?.stillShort == true
            },
            "Resume did not report the space: \(String(describing: space.pause))"
        )
        let refused = try await usedOverSeconds(3)
        say("Resume refused at 1200 MiB; processor time (ms per second): \(millis(refused))")
        #expect(refused.allSatisfy { $0 <= 5_000_000 })
        #expect(space.pause?.stage == .paused)

        // With room it wakes the guest, which runs again.
        try setFreeSpace(mebibytes: 3_000)
        space.resume()
        try #require(
            try await eventually(within: .seconds(120)) { space.pause == nil },
            "Resume did not wake the guest: \(String(describing: space.pause))"
        )
        #expect(!policy.spaceHeld)
        let running = try await usedOverSeconds(10)
        say("after Resume, processor time (ms per second): \(millis(running))")
        #expect(running.reduce(0, +) > 20_000_000)
        #expect(launcher.isActive)

        // Paused again, Shut down ends the run the way leaving the guest does.
        try setFreeSpace(mebibytes: 700)
        try #require(
            try await eventually(within: .seconds(180), every: .milliseconds(100)) {
                space.pause?.stage == .paused
            },
            "the guest was not paused a second time: \(String(describing: space.pause))"
        )
        say("paused again")
        space.shutDown()
        #expect(space.pause?.working == .shuttingDown)
        let ended = try await eventually(
            within: .seconds(180),
            every: .milliseconds(250)
        ) {
            if case .ended = launcher.phase { return true }
            return false
        }
        try #require(ended, "the run did not end: \(launcher.phase)")
        guard case let .ended(result) = launcher.phase else { return }
        // Android's runtime returns an error status for a host stop; the
        // screen says the guest shut down either way.
        let end = GuestRunEnd(result: result, requested: launcher.stopRequested)
        say("the run ended: status \(result), the screen says \"\(end.title)\"")
        #expect(end.title == "Guest shut down")
        #expect(space.pause == nil)
        #expect(!policy.spaceHeld)
    }

    /// The launch found the device short: nothing started, the screen says so,
    /// Check again stays refused while it is short and starts the guest once
    /// there is room.
    @Test(
        .enabled(if: SpacePauseGuestTests.startShort),
        .timeLimit(.minutes(15))
    )
    func aShortDeviceRefusesTheStartAndStartsOnceThereIsRoom() async throws {
        try #require(
            SpaceHarness.current.enabled,
            "KERNARIUM_SPACE_HARNESS=1 is not set"
        )
        defer { clearFreeSpace() }
        try #require(
            try await eventually(within: .seconds(120), every: .milliseconds(200)) {
                launcher.startShortage != nil
            },
            "the start was not refused: \(launcher.phase)"
        )
        let shortage = try #require(launcher.startShortage)
        say("start refused: \(shortage.message(device: EmulatorLauncher.deviceName))")
        #expect(launcher.phase == .idle)
        #expect(shortage.neededBytes == RunSpace.startReserveBytes)
        #expect(shortage.freeBytes < RunSpace.startReserveBytes)
        #expect(!space.holdsGuest)

        // Still short: asking again changes nothing.
        launcher.retryStart()
        #expect(launcher.startShortage != nil)
        #expect(launcher.phase == .idle)

        // With room the guest starts.
        try setFreeSpace(mebibytes: 8_192)
        launcher.retryStart()
        try #require(
            try await eventually(within: .seconds(600), every: .milliseconds(200)) {
                launcher.isActive
            },
            "the guest did not start once there was room: \(launcher.phase)"
        )
        #expect(launcher.startShortage == nil)
        say("started once there was room: \(launcher.guestName) runs")

        // A launch makes one run, and this one is over.
        try await Task.sleep(for: .seconds(5))
        let ended = try await eventually(
            within: .seconds(120),
            every: .milliseconds(250)
        ) {
            if launcher.phase == .running { launcher.stop() }
            if case .ended = launcher.phase { return true }
            return false
        }
        #expect(ended)
    }
}
