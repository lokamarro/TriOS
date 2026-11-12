// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
import UIKit
@testable import Andarium

/// Opt-in (`ANDARIUM_ROTATION_TESTS=1`, selecting only this suite): boots
/// Android and verifies that the host override preserves and restores locked
/// and unset guest rotation preferences. With `ANDARIUM_ROTATION_VISUAL=1` a
/// hands-on iPad check also covers rotation with Control Center's lock off and
/// transitions between a full-screen scene and a window; pure policy tests do
/// not establish the visible result.
@MainActor
struct RotationOverVsockTests {
    @Test(.enabled(if: ProcessInfo.processInfo.environment["ANDARIUM_ROTATION_VISUAL"] == "1"), .timeLimit(.minutes(20)))
    func fullScreenAndWindowedOwnershipFollowTheRealScene() async throws {
        try await withAndroidGuest { model, operations in
            try await checkSceneTransitions(model: model, operations: operations)
        }
    }

    func checkSceneTransitions(model: AppModel, operations: AdbGuestOperations) async throws {
        try await awaitScenePolicy(model, operations)
        try await run(GuestRotationPolicy.guest.command, operations)
        let original = try await settings(operations)
        do {
            try await run("wm fixed-to-user-rotation disabled; wm user-rotation free", operations)
            if model.hostDrivesGuestRotation { try await run(GuestRotationPolicy.host.command, operations) }
            let scene = try #require(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
            model.setMotionSensors(true)
            print("ROTATION: ready for hands-on check; rotate in full screen, enter a window, rotate, then return to full screen")
            var sawFullScreen = false
            var sawWindow = false
            var returned = false
            var previous = ""
            let deadline = ContinuousClock.now + .seconds(240)
            while ContinuousClock.now < deadline {
                let geometry = scene.effectiveGeometry
                let size = geometry.coordinateSpace.bounds.size
                let screen = scene.screen.bounds.size
                let full = abs(size.width - screen.width) < 0.5 && abs(size.height - screen.height) < 0.5
                let root = scene.windows.first { $0.isKeyWindow }?.rootViewController
                let summary = "full=\(full) requested=\(root?.prefersInterfaceOrientationLocked == true) delegated=\(root?.childForInterfaceOrientationLock != nil) locked=\(geometry.isInterfaceOrientationLocked) host=\(model.hostDrivesGuestRotation) interface=\(geometry.interfaceOrientation.rawValue)"
                if summary != previous {
                    print("ROTATION scene: \(summary)")
                    previous = summary
                }
                if full && geometry.isInterfaceOrientationLocked && !model.hostDrivesGuestRotation {
                    if sawWindow { returned = true; break }
                    sawFullScreen = true
                }
                if sawFullScreen && !full && model.hostDrivesGuestRotation,
                   try await settings(operations) == ["0", "0", "enabled"] {
                    sawWindow = true
                }
                try await Task.sleep(for: .seconds(1))
            }
            let completed = sawFullScreen && sawWindow && returned
            try #require(completed, "Expected locked full screen, host-owned window, then locked full screen")
            var restored = false
            let restoreDeadline = ContinuousClock.now + .seconds(10)
            while ContinuousClock.now < restoreDeadline {
                let current = try await settings(operations)
                if current[0] == "1" && current[2] == "disabled" { restored = true; break }
                try await Task.sleep(for: .milliseconds(200))
            }
            try #require(restored, "The full-screen guest's rotation preferences were not restored")
            try #require(model.motionSensorsProblem == nil, "\(model.motionSensorsProblem ?? "")")
            print("ROTATION: returned to locked full screen; verify rotation for another 20 seconds")
            try await Task.sleep(for: .seconds(20))
            try await restore(original, operations)
            print("ROTATION: full-screen/windowed/full-screen scene transitions passed")
        } catch {
            try? await run(GuestRotationPolicy.guest.command, operations)
            try? await restore(original, operations)
            throw error
        }
    }

    @Test(.enabled(if: ProcessInfo.processInfo.environment["ANDARIUM_ROTATION_TESTS"] == "1"), .timeLimit(.minutes(20)))
    func windowedRotationPreservesTheGuestPreferences() async throws {
        try await withAndroidGuest { model, operations in
            try await checkPreferences(model: model, operations: operations)
        }
    }

    func checkPreferences(model: AppModel, operations: AdbGuestOperations) async throws {
        // The real scene may already be windowed, with its own active
        // override. Start the command test from the saved guest choices.
        print("ROTATION: initial scene host ownership=\(model.hostDrivesGuestRotation)")
        try await awaitScenePolicy(model, operations)
        try await run(GuestRotationPolicy.guest.command, operations)
        let original = try await settings(operations)
        do {
            try await run("wm user-rotation lock 3; wm fixed-to-user-rotation disabled", operations)
            let locked = try await settings(operations)
            try #require(locked == ["0", "3", "disabled"])
            try await run(GuestRotationPolicy.host.command, operations)
            #expect(try await settings(operations) == ["0", "0", "enabled"])
            // Re-entering, including after an app restart, must retain the
            // first snapshot rather than save the override as a preference.
            try await run(GuestRotationPolicy.host.command, operations)
            try await run(GuestRotationPolicy.guest.command, operations)
            let restored = try await settings(operations)
            print("ROTATION: locked=\(locked), restored=\(restored)")
            try #require(restored == locked)

            try await run("settings delete system accelerometer_rotation; settings delete system user_rotation", operations)
            try await run(GuestRotationPolicy.host.command, operations)
            try await run(GuestRotationPolicy.guest.command, operations)
            let unset = try await settings(operations)
            print("ROTATION: restored unset preferences=\(unset)")
            try #require(unset == ["null", "null", "disabled"])
            let marker = try await operations.shell("test ! -e /data/local/tmp/andarium-rotation-state")
            #expect(marker.exitCode == 0)
            try await restore(original, operations)
            if model.hostDrivesGuestRotation { try await run(GuestRotationPolicy.host.command, operations) }
            print("ROTATION: host ownership, repeated entry, full-screen restoration and unset preferences passed")
        } catch {
            try? await run(GuestRotationPolicy.guest.command, operations)
            try? await restore(original, operations)
            throw error
        }
    }

    private func settings(_ operations: AdbGuestOperations) async throws -> [String] {
        let result = try await operations.shell(
            "settings get system accelerometer_rotation; settings get system user_rotation; wm fixed-to-user-rotation")
        try #require(result.exitCode == 0)
        let values = result.text.split(whereSeparator: \.isNewline).map(String.init)
        try #require(values.count == 3)
        try #require(["null", "0", "1"].contains(values[0]))
        try #require(["null", "0", "1", "2", "3"].contains(values[1]))
        try #require(["default", "disabled", "enabled", "enabled_if_no_auto_rotation"].contains(values[2]))
        return values
    }

    private func awaitScenePolicy(_ model: AppModel, _ operations: AdbGuestOperations) async throws {
        let deadline = ContinuousClock.now + .seconds(20)
        while ContinuousClock.now < deadline {
            if model.phase == .running(.normal) {
                if model.hostDrivesGuestRotation {
                    if try await settings(operations) == ["0", "0", "enabled"] {
                        let rotation = try await operations.shell("wm user-rotation")
                        if rotation.text.trimmingCharacters(in: .whitespacesAndNewlines) == "lock 0" { return }
                    }
                } else {
                    let marker = try await operations.shell("test ! -e /data/local/tmp/andarium-rotation-state")
                    if marker.exitCode == 0 { return }
                }
            }
            try await Task.sleep(for: .milliseconds(100))
        }
        throw AdbFailure(message: "The scene's initial rotation policy did not settle")
    }

    private func restore(_ values: [String], _ operations: AdbGuestOperations) async throws {
        let automatic = values[0] == "null" ? "settings delete system accelerometer_rotation"
            : "settings put system accelerometer_rotation \(values[0])"
        let angle = values[1] == "null" ? "settings delete system user_rotation"
            : "settings put system user_rotation \(values[1])"
        try await run("\(angle); \(automatic); wm fixed-to-user-rotation \(values[2])", operations)
    }

    private func run(_ command: String, _ operations: AdbGuestOperations) async throws {
        let result = try await operations.shell(command, limit: .seconds(15))
        try #require(result.exitCode == 0, "\(result.text)")
    }
}
