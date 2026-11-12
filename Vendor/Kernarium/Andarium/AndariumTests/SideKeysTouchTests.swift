// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The open side keys take touches only on the keys. A touch that begins
/// anywhere else reaches the guest's display, which reports it to the session,
/// and the session closes the keys. The tip beside the closed keys goes with
/// them.
@MainActor
@Suite(.serialized)
struct SideKeysTouchTests {
    fileprivate func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    fileprivate func descendants(of view: UIView) -> [UIView] {
        view.subviews + view.subviews.flatMap { descendants(of: $0) }
    }

    // MARK: The session's rule

    @Test func closingTheKeysChangesOnlyAnOpenSet() {
        let session = SessionState()
        #expect(!session.sideKeysOpen)
        session.closeSideKeys()
        #expect(!session.sideKeysOpen)
        session.sideKeysOpen = true
        session.closeSideKeys()
        #expect(!session.sideKeysOpen)
        // Every run starts with the keys closed.
        session.sideKeysOpen = true
        #expect(!SessionState().sideKeysOpen)
    }

    // MARK: The display's report

    @Test func aTouchThatBeginsOnTheDisplayIsReportedOnce() {
        let view = GuestMetalView(frame: CGRect(x: 0, y: 0, width: 200, height: 100), device: nil)
        var reports = 0
        view.touchDidBegin = { reports += 1 }
        view.touchesBegan([UITouch()], with: nil)
        #expect(reports == 1)
        // Moving and lifting are not beginnings.
        view.touchesMoved([UITouch()], with: nil)
        view.touchesEnded([UITouch()], with: nil)
        view.touchesCancelled([UITouch()], with: nil)
        #expect(reports == 1)
    }

    // MARK: The keys over the real display

    /// The production root with a run on screen: the guest's display under the
    /// session's overlay, and the points beside the keys a test touches.
    @MainActor
    private final class RunningSession {
        let model: AppModel
        let lease: HostWindow.Lease
        let root: UIView
        let guest: GuestMetalView
        /// The safe area the overlay lays out in, from its top-left corner.
        let width: CGFloat
        let height: CGFloat
        /// The height of the keys' centre, from `SideKeys`.
        let centreY: CGFloat
        let groupWidth = KeyMetrics.width * 2 + KeyMetrics.columnGap
        private let safe: UIEdgeInsets

        /// Holds the host window until `end()`.
        init(_ test: SideKeysTouchTests) async throws {
            let entry = AppEntry.fromArguments()
            let model = entry.makeModel()
            model.launchApplied = true
            model.driver = StubDriver(plan: nil)
            let lease = try await HostWindow.take()
            let controller = AndariumHostingController(rootView: RootView(entry: entry).environment(model), model: model)
            lease.window.rootViewController = controller
            model.phase = .running(.normal)
            let root = controller.view!
            var display: GuestMetalView?
            let found = await test.eventually {
                root.layoutIfNeeded()
                display = test.descendants(of: root).compactMap { $0 as? GuestMetalView }.first
                return display != nil && display!.bounds.width > 0
            }
            guard found, let display else {
                lease.give()
                throw Failure.noDisplay
            }
            let safe = root.safeAreaInsets
            let height = root.bounds.height - safe.top - safe.bottom
            self.model = model
            self.lease = lease
            self.root = root
            guest = display
            self.safe = safe
            width = root.bounds.width - safe.left - safe.right
            self.height = height
            centreY = min(max(118, height * 0.32), max(118, height - 118))
        }

        enum Failure: Error { case noDisplay }

        func end() { lease.give() }

        /// A point of the overlay's safe area in the window.
        func window(_ x: CGFloat, _ y: CGFloat) -> CGPoint {
            root.convert(CGPoint(x: safe.left + x, y: safe.top + y), to: lease.window)
        }

        func reachesGuest(_ point: CGPoint) -> Bool {
            guard let view = lease.window.hitTest(point, with: nil) else { return false }
            return view === guest || view.isDescendant(of: guest)
        }

        /// Beside the keys: across the screen and on the keys' own row.
        var beside: [CGPoint] {
            [
                window(width * 0.3, height * 0.5),
                window(width * 0.5, centreY),
                window(width - KeyMetrics.edgeGap - groupWidth - 40, centreY),
                window(width - 20, height * 0.9),
                window(width * 0.3, height * 0.1),
            ]
        }

        /// What the root presents over the session: the tip's popover.
        var presented: UIViewController? { lease.window.rootViewController?.presentedViewController }

        /// The tip's card while its popover is up, with the popover's margin.
        var tipCard: CGRect? {
            guard let view = presented?.viewIfLoaded else { return nil }
            return view.convert(view.bounds, to: lease.window).insetBy(dx: -12, dy: -12)
        }

        func onTip(_ point: CGPoint) -> Bool { tipCard?.contains(point) ?? false }

        /// A touch beside the shown tip reaches the guest. On a Mac an iPad
        /// app's popover keeps its dimming view, which takes every touch
        /// while the tip shows, so this holds on iOS alone.
        var besideTheTipReachesGuest: Bool { presented == nil || !ProcessInfo.processInfo.isiOSAppOnMac }
    }

    /// A point beside the open keys must reach the display (UIKit hands the
    /// touch to the display's view), and a point on a key must stay with the
    /// keys. Beside the closed keys only the tip, while it shows, takes a
    /// touch: on iOS only its card.
    @Test func theOpenKeysTakeNoTouchesBesideThem() async throws {
        let run = try await RunningSession(self)
        defer { run.end() }
        let model = run.model
        let width = run.width
        let far = run.beside[0]

        // The open group: Power's column at the edge and the guest keys beside
        // it, centred on the edge's fraction of the height, from `SideKeys`.
        let powerX = width - KeyMetrics.edgeGap - KeyMetrics.width / 2
        let settingsX = width - KeyMetrics.edgeGap - run.groupWidth + KeyMetrics.width / 2
        let groupTop = run.centreY - (KeyMetrics.keyHeight + KeyMetrics.rowGap + 2 * (KeyMetrics.keyHeight - 0.75) + 1.5) / 2
        let powerY = groupTop + KeyMetrics.keyHeight / 2
        let homeY = groupTop + KeyMetrics.keyHeight + KeyMetrics.rowGap + KeyMetrics.keyHeight * 1.5

        // Closed, as the baseline, beside the keys reaches the guest once the
        // tip's popover has finished appearing.
        model.session.sideKeysOpen = false
        try await Task.sleep(for: .milliseconds(600))
        run.root.layoutIfNeeded()
        if run.besideTheTipReachesGuest {
            #expect(await eventually { run.reachesGuest(far) }, "closed: \(far)")
            for point in run.beside where !run.onTip(point) { #expect(run.reachesGuest(point), "closed: \(point)") }
        }

        model.session.sideKeysOpen = true
        #expect(await eventually { run.presented == nil }, "the tip stayed beside the open keys")
        try await Task.sleep(for: .milliseconds(800))
        run.root.layoutIfNeeded()
        for point in run.beside { #expect(run.reachesGuest(point), "open: \(point)") }
        // On a key the touch stays with the keys.
        for (name, point) in [("Power", run.window(powerX, powerY)), ("Settings", run.window(settingsX, powerY)),
                              ("Home", run.window(settingsX, homeY)), ("Volume", run.window(powerX, homeY))] {
            #expect(!run.reachesGuest(point), "\(name) at \(point) reached the guest")
        }

        // A touch beginning on the display closes the keys, as the display's
        // report does.
        run.guest.touchesBegan([UITouch()], with: nil)
        #expect(await eventually { !model.session.sideKeysOpen })
        try await Task.sleep(for: .milliseconds(600))
        run.root.layoutIfNeeded()
        if run.besideTheTipReachesGuest {
            #expect(await eventually { run.reachesGuest(far) }, "closed again: \(far)")
            for point in run.beside where !run.onTip(point) { #expect(run.reachesGuest(point), "closed again: \(point)") }
        }
    }

    /// The tip beside the closed keys takes touches on its card (on iOS only
    /// there), and goes with the closed keys: when they open, nothing of it is
    /// left beside the open keys, and when the session ends, nothing of it is
    /// left over Home.
    /// The test bundle shows the tip in every session (`AndariumTestBundle`).
    @Test func theTipGoesWithTheClosedKeys() async throws {
        let run = try await RunningSession(self)
        defer { run.end() }
        let model = run.model

        try #require(await eventually { run.presented != nil }, "the tip did not show")
        let card = try #require(run.tipCard)
        let far = run.beside[0]
        let onCard = CGPoint(x: card.midX, y: card.midY)
        #expect(!run.reachesGuest(onCard), "a touch on the tip at \(onCard) reached the guest")
        // Once the popover is up, a touch beside its card reaches the guest.
        if run.besideTheTipReachesGuest {
            #expect(await eventually { run.reachesGuest(far) }, "the tip's popover took a touch beside it")
            for point in [far, run.window(run.width - 20, run.height * 0.9), run.window(run.width * 0.3, run.height * 0.1)] {
                #expect(run.reachesGuest(point), "tip shown: \(point)")
            }
        }

        model.session.sideKeysOpen = true
        #expect(await eventually { run.presented == nil }, "the tip stayed over the open keys")
        try await Task.sleep(for: .milliseconds(800))
        run.root.layoutIfNeeded()
        for point in run.beside { #expect(run.reachesGuest(point), "open: \(point)") }

        model.session.sideKeysOpen = false
        #expect(await eventually { run.presented != nil }, "the tip did not come back with the closed keys")
        model.phase = .home
        #expect(await eventually { run.presented == nil }, "the tip stayed after the session ended")
    }
}
