// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import Testing
import UIKit
@testable import Kernarium

/// Whether the guest view clears the window controls follows whether its
/// scene covers the screen. The system's corner offset keeps describing a
/// window after Multitasking switches a running scene to Full Screen Apps,
/// so the application decides from the scene's geometry. iPadOS draws the
/// controls over the content of a window, a tile and a Slide Over window,
/// and moves them to the menu bar of a window zoomed to the screen.
struct SceneCoverageTests {
    private let landscape = CGSize(width: 1133, height: 744)
    private let portrait = CGSize(width: 744, height: 1133)

    @Test
    func aSceneAsBigAsTheScreenCoversIt() {
        // Full Screen Apps, and a window zoomed to the screen.
        #expect(SceneCoverage.covers(sceneSize: landscape, screenSize: landscape))
        #expect(SceneCoverage.covers(sceneSize: portrait, screenSize: portrait))
        #expect(SceneCoverage.covers(
            sceneSize: CGSize(width: 1133.2, height: 743.9),
            screenSize: landscape
        ))
    }

    @Test
    func aWindowATileAndASlideOverWindowDoNotCoverTheScreen() {
        for size in [
            CGSize(width: 1007.5, height: 700),
            CGSize(width: 561.5, height: 744),
            CGSize(width: 375, height: 565),
            CGSize(width: 1132, height: 744),
            CGSize(width: 1133, height: 743),
        ] {
            #expect(!SceneCoverage.covers(sceneSize: size, screenSize: landscape))
        }
    }

    @Test
    func aSceneTheSizeOfAScreenInTheOtherOrientationDoesNotCoverIt() {
        #expect(!SceneCoverage.covers(sceneSize: portrait, screenSize: landscape))
    }

    @Test
    func theCornerOffsetStaysOffWhereTheSceneCoversTheScreen() {
        #expect(SceneCoverage.cornerOffsetEdges(coversScreen: true).isEmpty)
        #expect(SceneCoverage.cornerOffsetEdges(coversScreen: false) == .top)
    }

    @Test
    @MainActor
    func theViewReportsTheCoverageOfItsSceneOnceAndAgainOnlyOnAChange() async throws {
        let scene = try windowScene()
        let window = UIWindow(windowScene: scene)
        defer { window.isHidden = true }
        let root = UIViewController()
        window.rootViewController = root
        window.isHidden = false

        var reports: [Bool] = []
        let view = SceneCoverageView()
        view.report = { reports.append($0) }
        view.frame = root.view.bounds
        root.view.addSubview(view)

        #expect(try await eventually { !reports.isEmpty })
        #expect(reports == [SceneCoverage.covers(scene)])

        // A layout that leaves the scene's geometry as it was reports
        // nothing.
        view.setNeedsLayout()
        view.layoutIfNeeded()
        root.view.setNeedsLayout()
        root.view.layoutIfNeeded()
        try await Task.sleep(for: .milliseconds(200))
        #expect(reports.count == 1)
    }
}
