// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// When the guest keeps its display mode while the host view changes size.
///
/// A view-controller transition holds the mode from its start to its
/// completion. An iPadOS window resize is one interactive gesture that runs
/// many such transitions, one per size the window passes through, and it holds
/// the mode until the gesture ends, as an AppKit live resize does. The guest
/// then sees only the size the gesture settles at, not each pause on the way.
struct GuestResizeHold {
    enum Action: Equatable {
        case none
        case begin
        case finish
    }

    private var generation: UInt64 = 0
    private var transitionInFlight = false
    private var interactive = false

    /// A transition starts; returns the token its completion presents.
    mutating func transitionWillBegin() -> (token: UInt64, action: Action) {
        generation &+= 1
        transitionInFlight = true
        return (generation, .begin)
    }

    /// A transition completed. A superseded transition never finishes the
    /// hold, and neither does any transition during an interactive resize.
    mutating func transitionDidComplete(token: UInt64) -> Action {
        guard token == generation else { return .none }
        transitionInFlight = false
        return interactive ? .none : .finish
    }

    /// The window scene's interactive-resize state changed. The end of a
    /// gesture finishes the hold unless the gesture's last transition is
    /// still running, which finishes it on completion.
    mutating func interactiveResizeChanged(_ resizing: Bool) -> Action {
        guard resizing != interactive else { return .none }
        interactive = resizing
        if resizing { return .begin }
        return transitionInFlight ? .none : .finish
    }
}
