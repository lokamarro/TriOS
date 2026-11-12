// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// A notice read at moments `seconds` after a start. A class, so that an
/// expectation can take a reading (`#expect` cannot call a mutating member).
private final class Watch {
    var notice = GraphicsNotice()
    private let start = ContinuousClock.now

    private func at(_ seconds: Int) -> ContinuousClock.Instant {
        start.advanced(by: .seconds(seconds))
    }

    /// The count read at `seconds`: whether a notice appeared with it.
    func read(_ refusals: UInt64, at seconds: Int) -> Bool {
        notice.record(refusals: refusals, at: at(seconds))
    }

    func expire(at seconds: Int) {
        notice.expire(at: at(seconds))
    }
}

struct GraphicsNoticeTests {
    @Test func nothingIsRefusedNothingIsShown() {
        let watch = Watch()
        for second in stride(from: 0, through: 600, by: 2) {
            #expect(!watch.read(0, at: second))
        }
        #expect(!watch.notice.isShowing)
    }

    @Test func theFirstReadingIsABaselineNotAnAnnouncement() {
        // The count started before the application looked: those refusals were
        // not announced, and a later one still is.
        let watch = Watch()
        #expect(!watch.read(9, at: 0))
        #expect(!watch.notice.isShowing)
        #expect(!watch.read(9, at: 2))
        #expect(watch.read(10, at: 4))
        #expect(watch.notice.isShowing)
    }

    @Test func aRefusalShowsTheNoticeOnce() {
        let watch = Watch()
        _ = watch.read(0, at: 0)
        #expect(watch.read(1, at: 2))
        #expect(watch.notice.isShowing)
        // The same count again is not a new refusal.
        #expect(!watch.read(1, at: 4))
        #expect(watch.notice.isShowing)
    }

    @Test func aBurstIsOneNoticeHoweverManyRefusalsItHolds() {
        let watch = Watch()
        _ = watch.read(0, at: 0)
        #expect(watch.read(3, at: 2))
        // Each reading finds more, every few seconds: still the same burst.
        var count: UInt64 = 3
        for second in stride(from: 4, through: 120, by: 4) {
            count += 5
            #expect(!watch.read(count, at: second), "second \(second)")
        }
    }

    @Test func aDismissedBurstDoesNotComeBack() {
        let watch = Watch()
        _ = watch.read(0, at: 0)
        #expect(watch.read(1, at: 2))
        watch.notice.dismiss()
        #expect(!watch.notice.isShowing)
        #expect(!watch.read(2, at: 4), "the burst the person dismissed announced itself again")
        #expect(!watch.notice.isShowing)
    }

    @Test func aRefusalAfterAQuietMinuteStartsANewBurst() {
        let watch = Watch()
        _ = watch.read(0, at: 0)
        #expect(watch.read(1, at: 10))
        watch.notice.dismiss()
        // 59 s after the last refusal is the same burst, and a burst goes on
        // while refusals come less than a minute apart.
        for second in stride(from: 12, through: 68, by: 2) {
            _ = watch.read(1, at: second)
        }
        #expect(!watch.read(2, at: 69))
        #expect(!watch.read(3, at: 128))
        // A whole quiet minute ends it.
        #expect(watch.read(4, at: 188))
        #expect(watch.notice.isShowing)
    }

    @Test func theQuietMinuteCountsFromTheLastRefusalNotTheNotice() {
        let watch = Watch()
        _ = watch.read(0, at: 0)
        #expect(watch.read(1, at: 2))
        #expect(!watch.read(2, at: 50))
        // 59 s after the notice but 11 s after the last refusal.
        #expect(!watch.read(3, at: 61))
    }

    @Test func theNoticeGoesByItselfAfterItsTime() {
        let watch = Watch()
        _ = watch.read(0, at: 0)
        #expect(watch.read(1, at: 2))
        _ = watch.read(1, at: 31)
        #expect(watch.notice.isShowing, "the notice went before its time")
        _ = watch.read(1, at: 32)
        #expect(!watch.notice.isShowing)
        // Going by itself is not the end of the burst.
        #expect(!watch.read(2, at: 34))
        #expect(!watch.notice.isShowing)
    }

    @Test func aCountThatWentBackwardsIsANewBaseline() {
        let watch = Watch()
        _ = watch.read(0, at: 0)
        #expect(watch.read(5, at: 2))
        watch.notice.dismiss()
        // A new display counts from zero again: nothing is refused yet.
        #expect(!watch.read(0, at: 200))
        #expect(watch.read(1, at: 210))
    }

    @Test func dismissingWhatIsNotShownChangesNothing() {
        let watch = Watch()
        watch.notice.dismiss()
        #expect(!watch.notice.isShowing)
        watch.expire(at: 100)
        #expect(!watch.notice.isShowing)
    }

    @Test func theWordsAreShortAndPlain() {
        #expect(GraphicsNotice.title.count <= 40)
        #expect(GraphicsNotice.detail.count <= 100)
        #expect(!GraphicsNotice.dismissLabel.isEmpty)
    }
}
