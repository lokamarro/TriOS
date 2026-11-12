// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The real scene root's session lock and deferred edges, and the built
/// bundle's portrait-only iPhone declaration. Only a device shows how the
/// system answers the swipes (docs/testing.md, "Edge gestures").
@MainActor
@Suite(.serialized)
struct SessionOrientationHostingTests {
    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    private static var applicationState: String {
        switch UIApplication.shared.applicationState {
        case .active: "active"
        case .inactive: "inactive (a test or the system took it out of the foreground)"
        case .background: "in the background"
        @unknown default: "in an unknown state"
        }
    }

    @Test func theSceneRootLocksOnlyTheGuestSession() async throws {
        let entry = AppEntry.fromArguments()
        let model = entry.makeModel()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let scene = lease.scene
        let controller = AndariumHostingController(rootView: Color.black, model: model)
        lease.window.rootViewController = controller
        let phone = UIDevice.current.userInterfaceIdiom == .phone
        #expect(controller.supportedInterfaceOrientations == (phone ? .portrait : .all))
        #expect(!controller.prefersInterfaceOrientationLocked)
        model.phase = .running(.normal)
        let size = scene.effectiveGeometry.coordinateSpace.bounds.size
        let screen = scene.screen.bounds.size
        let fullScreen = abs(size.width - screen.width) < 0.5 && abs(size.height - screen.height) < 0.5
        if !phone && fullScreen {
            #expect(await eventually { controller.prefersInterfaceOrientationLocked })
            #expect(
                await eventually { scene.effectiveGeometry.isInterfaceOrientationLocked },
                "the scene applies the lock only while the application is active; it is \(Self.applicationState)")
            #expect(await eventually { !model.hostDrivesGuestRotation })
        } else {
            #expect(!controller.prefersInterfaceOrientationLocked)
            #expect(await eventually { model.hostDrivesGuestRotation == !phone })
        }
        model.phase = .home
        #expect(await eventually { !controller.prefersInterfaceOrientationLocked && !model.hostDrivesGuestRotation })
    }

    @Test func theSceneRootDefersTheGuestsEdgesOnlyInANormalRun() async throws {
        let entry = AppEntry.fromArguments()
        let model = entry.makeModel()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let scene = lease.scene
        let controller = AndariumHostingController(rootView: Color.black, model: model)
        lease.window.rootViewController = controller
        // Outside a run the root keeps SwiftUI's preference, which never
        // takes the top or the bottom edge from the system.
        let outside = controller.preferredScreenEdgesDeferringSystemGestures
        #expect(!outside.contains(.top) && !outside.contains(.bottom))
        model.phase = .running(.normal)
        let size = scene.effectiveGeometry.coordinateSpace.bounds.size
        let screen = scene.screen.bounds.size
        let fullScreen = abs(size.width - screen.width) < 0.5 && abs(size.height - screen.height) < 0.5
        if fullScreen {
            #expect(await eventually { controller.preferredScreenEdgesDeferringSystemGestures == .all })
            #expect(controller.childForScreenEdgesDeferringSystemGestures == nil)
        } else {
            try await Task.sleep(for: .milliseconds(300))
            #expect(controller.preferredScreenEdgesDeferringSystemGestures == outside)
        }
        model.phase = .running(.recovery)
        #expect(await eventually { controller.preferredScreenEdgesDeferringSystemGestures == outside })
        model.phase = .home
        #expect(await eventually { controller.preferredScreenEdgesDeferringSystemGestures == outside })
    }

    @Test func theGuestKeepsThreeFingersWhileItHasTheKeyboardFocus() {
        let view = GuestMetalView(frame: .zero, device: nil)
        #expect(view.editingInteractionConfiguration == .none)
    }

    @Test func theSceneRootTellsTheDriverWhenTheGeometryChanges() async throws {
        let entry = AppEntry.fromArguments()
        let model = entry.makeModel()
        let driver = StubDriver(plan: nil)
        model.driver = driver
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = AndariumHostingController(rootView: Color.black, model: model)
        lease.window.rootViewController = controller
        #expect(await eventually { driver.geometryChanges > 0 })
        // Let the first layouts settle, then report a storm of geometry
        // updates within one turn: the driver hears of it, and not once for
        // each of them.
        try await Task.sleep(for: .milliseconds(500))
        let before = driver.geometryChanges
        for _ in 0..<50 { controller.sceneGeometryDidChange() }
        #expect(await eventually { driver.geometryChanges > before })
        try await Task.sleep(for: .milliseconds(200))
        #expect(driver.geometryChanges - before <= 5)
    }

    @Test func theBundleAllowsOnlyPortraitOnIPhone() throws {
        // Read the declarations themselves: Bundle's device-qualified lookup
        // resolves orientation keys for the iPad running this test.
        let data = try Data(contentsOf: Bundle.main.bundleURL.appendingPathComponent("Info.plist"))
        let info = try #require(try PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any])
        let orientations = info["UISupportedInterfaceOrientations~iphone"] ?? info["UISupportedInterfaceOrientations"]
        #expect(orientations as? [String] == ["UIInterfaceOrientationPortrait"])
        #expect((info["UISupportedInterfaceOrientations~ipad"] as? [String])?.count == 4)
    }
}
