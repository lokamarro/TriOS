// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The model and the runtime it drives belong to the process, not to a
/// scene: UIKit disconnects a scene it releases and connects one again, and
/// a Mac connects a scene of its own for the menu bar.
@MainActor
@Suite(.serialized)
struct SceneLifetimeTests {
    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(20)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    private var windowScenes: [UIWindowScene] {
        UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
    }

    private func applicationScene() async throws -> UIWindowScene {
        try #require(await eventually { windowScenes.contains { $0.delegate is AndariumSceneDelegate } })
        return try #require(windowScenes.first { $0.delegate is AndariumSceneDelegate })
    }

    @Test func everySceneConnectionShowsTheProcessModel() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let scene = try await applicationScene()
        let launched = try #require((scene.delegate as? AndariumSceneDelegate)?.model)
        #expect(launched === AndariumProcess.shared.model)
        let first = AndariumSceneDelegate()
        let second = AndariumSceneDelegate()
        first.connect(scene)
        second.connect(scene)
        defer {
            for delegate in [first, second] {
                delegate.window?.isHidden = true
                delegate.window = nil
            }
        }
        #expect(first.model === launched)
        #expect(second.model === launched)
    }

    #if !APP_STORE
    @Test func theLaunchsWorkRunsOncePerModelWhateverTheSceneCount() async throws {
        let host = HostDevice(
            name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        let entry = AppEntry(overrides: LaunchOverrides(environment: ["ANDARIUM_ERASE": "1"], processorCount: 4))
        _ = try await applicationScene()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let window = lease.window
        // A scene connecting again shows the same model in a new root.
        for connection in 1...2 {
            window.rootViewController = UIHostingController(rootView: RootView(entry: entry).environment(model))
            try #require(await eventually { model.launchApplied })
            try await Task.sleep(for: .milliseconds(500))
            #expect(driver.erased == 1, "connection \(connection)")
        }
    }
    #endif
}
