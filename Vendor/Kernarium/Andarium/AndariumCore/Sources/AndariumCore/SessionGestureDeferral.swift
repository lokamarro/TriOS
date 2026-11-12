// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Screen edges, with UIKit's `UIRectEdge` raw values so the application
/// converts one into the other with `init(rawValue:)`.
public struct ScreenEdges: OptionSet, Hashable, Sendable {
    public let rawValue: UInt

    public init(rawValue: UInt) { self.rawValue = rawValue }

    public static let top = ScreenEdges(rawValue: 1 << 0)
    public static let left = ScreenEdges(rawValue: 1 << 1)
    public static let bottom = ScreenEdges(rawValue: 1 << 2)
    public static let right = ScreenEdges(rawValue: 1 << 3)
    public static let all: ScreenEdges = [.top, .left, .bottom, .right]
}

/// The screen edges whose system gestures a running guest defers.
///
/// On a deferred edge the system gives the first swipe to the application and
/// shows its indicator; a second swipe while the indicator shows reaches the
/// system. A normal run in a scene that fills the screen defers all four
/// edges, on an iPhone and an iPad alike, so a stray swipe from an edge
/// reaches Android (its notification shade, its navigation gestures) or shows
/// the system's indicator instead of leaving Andarium or opening Control
/// Center. The guest's display stays inside the safe area: only the
/// preference changes, not the layout. The bottom edge holds only while the
/// home indicator is visible, which the session's view leaves so. Outside a
/// normal run, and in a window that does not fill the screen, the session
/// takes no part and the screen keeps its own preference.
public struct SessionGestureDeferral: Equatable, Sendable {
    /// The edges to defer, or nil where the session does not decide.
    public let deferredEdges: ScreenEdges?

    public init(phase: Phase, sceneSize: CGSize, screenSize: CGSize) {
        let fullScreen = abs(sceneSize.width - screenSize.width) < 0.5
            && abs(sceneSize.height - screenSize.height) < 0.5
        guard case .running(.normal) = phase, fullScreen else {
            deferredEdges = nil
            return
        }
        deferredEdges = .all
    }
}
