// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

// Orientation ownership for phones, full-screen and windowed iPads and the loss
// of a scene lock (SessionOrientationTests), and coalesced guest commands,
// partial failures and stale completions after a guest restart
// (GuestRotationControllerTests).
struct SessionOrientationTests {
    private let screen = CGSize(width: 1024, height: 768)

    private func policy(
        _ phase: Phase = .running(.normal), phone: Bool = false,
        size: CGSize? = nil, locked: Bool = false
    ) -> SessionOrientationPolicy {
        SessionOrientationPolicy(
            phase: phase, isPhone: phone, sceneSize: size ?? screen,
            screenSize: screen, interfaceLocked: locked)
    }

    @Test func fullScreenWaitsForTheActualLock() {
        #expect(policy().requestInterfaceLock)
        #expect(policy().hostDrivesGuestRotation)
        #expect(policy(locked: true).requestInterfaceLock)
        #expect(!policy(locked: true).hostDrivesGuestRotation)
        // Losing the lock to an overlapping scene restores the host policy.
        #expect(policy(locked: false).hostDrivesGuestRotation)
    }

    @Test func windowsUseHostRotationEvenIfThePreviousSceneWasLocked() {
        let window = CGSize(width: 600, height: 768)
        #expect(!policy(size: window, locked: true).requestInterfaceLock)
        #expect(policy(size: window, locked: true).hostDrivesGuestRotation)
        #expect(policy(size: CGSize(width: 768, height: 1024)).hostDrivesGuestRotation)
    }

    @Test func phonesAndScreensOutsideTheSessionDoNotOverrideGuestRotation() {
        #expect(!policy(phone: true).requestInterfaceLock)
        #expect(!policy(phone: true).hostDrivesGuestRotation)
        for phase in [Phase.home, .failed] {
            #expect(!policy(phase).requestInterfaceLock)
            #expect(!policy(phase).hostDrivesGuestRotation)
        }
        #expect(policy(.booting(.normal)).requestInterfaceLock)
        #expect(policy(.running(.recovery)).requestInterfaceLock)
    }
}

@MainActor
struct GuestRotationControllerTests {
    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(3)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(5))
        }
        return condition()
    }

    @Test func aResizeDuringACommandIsAppliedAfterThatCommand() async throws {
        var commands: [GuestRotationPolicy] = []
        var release: CheckedContinuation<Void, Never>?
        let controller = GuestRotationController(apply: { policy in
            commands.append(policy)
            if commands.count == 1 { await withCheckedContinuation { release = $0 } }
        }, changed: {}, failure: { Issue.record("unexpected failure: \($0)") })
        controller.update(policy: .host, ready: false)
        #expect(commands.isEmpty)
        controller.update(policy: .host, ready: true)
        try #require(await eventually { release != nil })
        controller.update(policy: .guest, ready: true)
        #expect(commands == [.host])
        release?.resume()
        #expect(await eventually { controller.applied == .guest })
        #expect(commands == [.host, .guest])
        controller.update(policy: .guest, ready: true)
        #expect(commands.count == 2)
    }

    @Test func aPreviousBootCannotCompleteTheNewBootsPolicy() async throws {
        var commands = 0
        var release: CheckedContinuation<Void, Never>?
        let controller = GuestRotationController(apply: { _ in
            commands += 1
            if commands == 1 { await withCheckedContinuation { release = $0 } }
        }, changed: {}, failure: { Issue.record("unexpected failure: \($0)") })
        controller.update(policy: .host, ready: true)
        try #require(await eventually { release != nil })
        controller.update(policy: .guest, ready: false)
        controller.update(policy: .guest, ready: true)
        try #require(await eventually { controller.applied == .guest })
        release?.resume()
        await Task.yield()
        #expect(controller.applied == .guest)
        #expect(commands == 2)
    }

    @Test func aPartialFailureIsReconciledAndDoesNotRetryOnEveryLayout() async throws {
        enum Failure: Error { case command }
        var commands: [GuestRotationPolicy] = []
        var failures = 0
        let controller = GuestRotationController(apply: { policy in
            commands.append(policy)
            if policy == .host { throw Failure.command }
        }, changed: {}, failure: { _ in failures += 1 })
        controller.update(policy: .guest, ready: true)
        try #require(await eventually { controller.applied == .guest })
        controller.update(policy: .host, ready: true)
        try #require(await eventually { failures == 1 })
        #expect(controller.applied == nil)
        controller.update(policy: .host, ready: true)
        #expect(commands == [.guest, .host])
        controller.update(policy: .guest, ready: true)
        #expect(await eventually { commands == [.guest, .host, .guest] })
    }
}
