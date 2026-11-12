// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// Whether a window scene fills its screen, and so has no window controls
/// over its content: Full Screen Apps, and a window zoomed to the screen,
/// whose controls move to the menu bar. A window, tile or Slide Over scene
/// is smaller than the screen.
enum SceneCoverage {
    static func covers(sceneSize: CGSize, screenSize: CGSize) -> Bool {
        // A scene is a few hundredths of a point off the screen at most,
        // where it fills it.
        abs(sceneSize.width - screenSize.width) < 0.5
            && abs(sceneSize.height - screenSize.height) < 0.5
    }

    static func covers(_ scene: UIWindowScene) -> Bool {
        covers(
            sceneSize: scene.effectiveGeometry.coordinateSpace.bounds.size,
            screenSize: scene.screen.bounds.size
        )
    }

    /// The edges whose corner offset the guest view takes: the top, which
    /// the window controls overlay, unless the scene covers the screen.
    static func cornerOffsetEdges(coversScreen: Bool) -> Edge.Set {
        coversScreen ? [] : .top
    }
}

/// Reports, now and whenever the scene's geometry changes, whether the
/// scene the view is in covers its screen. The system's corner offset keeps
/// describing a window after Multitasking switches the scene to Full Screen
/// Apps, so the offset cannot be the judge of whether there are controls.
struct SceneCoverageReader: UIViewRepresentable {
    let report: @MainActor (Bool) -> Void

    func makeUIView(context: Context) -> SceneCoverageView {
        let view = SceneCoverageView()
        view.isUserInteractionEnabled = false
        view.report = report
        return view
    }

    func updateUIView(_ view: SceneCoverageView, context: Context) {
        view.report = report
    }
}

final class SceneCoverageView: UIView {
    var report: (@MainActor (Bool) -> Void)?
    private var observation: NSKeyValueObservation?
    private var reported: Bool?

    override func didMoveToWindow() {
        super.didMoveToWindow()
        observation = nil
        guard let scene = window?.windowScene else { return }
        observation = scene.observe(
            \.effectiveGeometry, options: [.initial, .new]
        ) { [weak self] scene, _ in
            MainActor.assumeIsolated { self?.update(from: scene) }
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        if let scene = window?.windowScene {
            update(from: scene)
        }
    }

    private func update(from scene: UIWindowScene) {
        let covers = SceneCoverage.covers(scene)
        guard covers != reported else { return }
        reported = covers
        // SwiftUI state is not written from inside its own layout pass.
        DispatchQueue.main.async { [weak self] in
            self?.report?(covers)
        }
    }
}
