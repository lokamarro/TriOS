// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
@testable import Kernarium

/// What the application does with the runtime's restart events (runtime API
/// 14): the numeric events become cases, and the indicator that shows a guest
/// between two boots follows them without a notice for a restart the guest
/// asked for.
@MainActor
struct GuestRestartEventTests {
    @Test
    func theRuntimesEventsBecomeCases() {
        #expect(
            GuestRestartEvent(
                event: UInt32(KERNARIUM_RUNTIME_RESTART_BEGAN),
                restartCount: 3
            ) == .began(restart: 3)
        )
        #expect(
            GuestRestartEvent(
                event: UInt32(KERNARIUM_RUNTIME_RESTART_BOOTED),
                restartCount: 3
            ) == .booted(restart: 3)
        )
    }

    /// A newer runtime may report events this application does not know; the
    /// application ignores them rather than guessing.
    @Test
    func anEventTheApplicationDoesNotKnowIsIgnored() {
        #expect(GuestRestartEvent(event: 99, restartCount: 1) == nil)
        #expect(GuestRestartEvent(event: UInt32.max, restartCount: 0) == nil)
    }

    @Test
    func theIndicatorShowsTheBootBetweenTheTwoEvents() {
        var indicator = GuestRestartIndicator()
        #expect(!indicator.restarting)

        indicator.apply(.began(restart: 1))
        #expect(indicator.restarting)
        #expect(indicator.restarts == 1)

        indicator.apply(.booted(restart: 1))
        #expect(!indicator.restarting)

        indicator.apply(.began(restart: 2))
        #expect(indicator.restarting)
        indicator.apply(.booted(restart: 2))
        #expect(!indicator.restarting)
        #expect(indicator.restarts == 2)
    }

    /// The events cross threads before they reach the main queue, so the
    /// answer of an earlier boot may land after the next restart began; it
    /// must not hide the badge of the boot that is still under way.
    @Test
    func theAnswerOfAnEarlierBootDoesNotEndALaterRestart() {
        var indicator = GuestRestartIndicator()
        indicator.apply(.began(restart: 1))
        indicator.apply(.began(restart: 2))
        indicator.apply(.booted(restart: 1))
        #expect(indicator.restarting)
        indicator.apply(.booted(restart: 2))
        #expect(!indicator.restarting)
    }

    @Test
    func aRunThatEndedRestartsNothingMore() {
        var indicator = GuestRestartIndicator()
        indicator.apply(.began(restart: 4))
        indicator.runEnded()
        #expect(!indicator.restarting)
        // The restart count is kept: it is what the run did.
        #expect(indicator.restarts == 4)
    }
}
