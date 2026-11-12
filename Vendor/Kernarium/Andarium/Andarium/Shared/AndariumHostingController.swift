// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Observation
import SwiftUI
import UIKit

@MainActor
protocol SceneGeometryObserver: AnyObject {
    func sceneGeometryDidChange()
}

/// The scene's root controller owns the orientation preference and, in a
/// run, the screen edges that defer system gestures. A child of SwiftUI's
/// default root cannot reliably supply these scene-wide preferences.
final class AndariumHostingController<Content: View>: UIHostingController<Content>, SceneGeometryObserver {
    private let model: AppModel
    private var lockRequested = false
    /// The run's deferred edges; nil leaves SwiftUI's preference in charge.
    private var deferredEdges: ScreenEdges?
    private var updateScheduled = false

    init(rootView: Content, model: AppModel) {
        self.model = model
        super.init(rootView: rootView)
        observePhase()
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override var supportedInterfaceOrientations: UIInterfaceOrientationMask {
        UIDevice.current.userInterfaceIdiom == .phone ? .portrait : .all
    }

    override var prefersInterfaceOrientationLocked: Bool { lockRequested }

    // The scene policy belongs to this root, including when SwiftUI embeds
    // a UIKit display controller whose default lock preference is false.
    override var childForInterfaceOrientationLock: UIViewController? { nil }

    // SwiftUI defers the left and right edges unless a view says otherwise,
    // and on Home it hands the choice to a navigation child. In a run the
    // root decides alone. A presented sheet supplies its own preference.
    override var preferredScreenEdgesDeferringSystemGestures: UIRectEdge {
        guard let deferredEdges else { return super.preferredScreenEdgesDeferringSystemGestures }
        return UIRectEdge(rawValue: deferredEdges.rawValue)
    }

    override var childForScreenEdgesDeferringSystemGestures: UIViewController? {
        deferredEdges == nil ? super.childForScreenEdgesDeferringSystemGestures : nil
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        scheduleUpdate()
    }

    func sceneGeometryDidChange() {
        scheduleUpdate()
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        scheduleUpdate()
    }

    private func observePhase() {
        withObservationTracking {
            _ = model.phase
        } onChange: { [weak self] in
            Task { @MainActor [weak self] in
                self?.observePhase()
                self?.scheduleUpdate()
            }
        }
    }

    private func scheduleUpdate() {
        guard !updateScheduled else { return }
        updateScheduled = true
        Task { @MainActor [weak self] in
            guard let self else { return }
            updateScheduled = false
            guard let scene = view.window?.windowScene else { return }
            let size = scene.effectiveGeometry.coordinateSpace.bounds.size
            let policy = SessionOrientationPolicy(
                phase: model.phase, isPhone: UIDevice.current.userInterfaceIdiom == .phone,
                sceneSize: size, screenSize: scene.screen.bounds.size,
                interfaceLocked: scene.effectiveGeometry.isInterfaceOrientationLocked)
            if policy.requestInterfaceLock != lockRequested {
                lockRequested = policy.requestInterfaceLock
                setNeedsUpdateOfPrefersInterfaceOrientationLocked()
            }
            model.hostDrivesGuestRotation = policy.hostDrivesGuestRotation
            let deferral = SessionGestureDeferral(
                phase: model.phase, sceneSize: size, screenSize: scene.screen.bounds.size)
            if deferral.deferredEdges != deferredEdges {
                deferredEdges = deferral.deferredEdges
                setNeedsUpdateOfScreenEdgesDeferringSystemGestures()
            }
            // One call per turn, whatever the number of layouts and geometry
            // updates that scheduled it.
            model.driver?.sceneGeometryDidChange()
        }
    }
}
