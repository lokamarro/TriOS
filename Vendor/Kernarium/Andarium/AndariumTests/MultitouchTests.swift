// SPDX-License-Identifier: GPL-2.0-or-later
import Testing
import UIKit
@testable import Andarium

/// Independent finger paths and slots, cancellation, viewport crossings, the
/// ten-contact limit and releasing all contacts. The host `virtio-input-protocol`
/// unit test covers the advertised multitouch axes, tracking-ID lifetimes and
/// wraparound, and legacy primary-finger coordinates.
@MainActor
struct MultitouchTests {
    private func geometry() throws -> GuestTouchGeometry {
        try #require(GuestTouchGeometry(
            coordinateSpace: GuestInputCoordinateSpace(
                size: CGSize(width: 1000, height: 1000)
            ),
            boundsSize: CGSize(width: 200, height: 100),
            drawableSize: CGSize(width: 200, height: 100),
            contentSize: CGSize(width: 100, height: 100)
        ))
    }

    @Test
    func pinchKeepsIndependentSlotsWhenOneFingerLifts() throws {
        let geometry = try geometry()
        var contacts = GuestTouchContacts<Int>()
        let first = try #require(contacts.begin(
            1, at: CGPoint(x: 80, y: 50), geometry: geometry
        ).first)
        let second = try #require(contacts.begin(
            2, at: CGPoint(x: 120, y: 50), geometry: geometry
        ).first)
        #expect(first.slot != second.slot)
        #expect(first.event == GuestTouchEvent(x: 300, y: 500, pressed: true))
        #expect(second.event == GuestTouchEvent(x: 700, y: 500, pressed: true))
        #expect(contacts.move(
            1, through: [CGPoint(x: 60, y: 50)], geometry: geometry
        ) == [GuestTouchContactEvent(
            slot: first.slot,
            event: GuestTouchEvent(x: 100, y: 500, pressed: true)
        )])
        #expect(contacts.cancel(1) == [GuestTouchContactEvent(
            slot: first.slot,
            event: GuestTouchEvent(
                x: 100, y: 500, pressed: false, cancelled: true
            )
        )])
        #expect(contacts.move(
            2, through: [CGPoint(x: 140, y: 50)], geometry: geometry
        ) == [GuestTouchContactEvent(
            slot: second.slot,
            event: GuestTouchEvent(x: 900, y: 500, pressed: true)
        )])
        let third = try #require(contacts.begin(
            3, at: CGPoint(x: 90, y: 40), geometry: geometry
        ).first)
        #expect(third.slot == first.slot)
        #expect(contacts.cancelAll().map(\.slot).sorted() ==
            [second.slot, third.slot].sorted())
        #expect(contacts.cancelAll().isEmpty)
        #expect(contacts.move(
            2, through: [CGPoint(x: 100, y: 50)], geometry: geometry
        ).isEmpty)
    }

    @Test
    func viewportCrossingsAndCancellationAffectOnlyTheirContact() throws {
        let geometry = try geometry()
        var contacts = GuestTouchContacts<Int>()
        _ = contacts.begin(1, at: CGPoint(x: 80, y: 50), geometry: geometry)
        _ = contacts.begin(2, at: CGPoint(x: 120, y: 50), geometry: geometry)
        let crossing = contacts.move(
            1,
            through: [CGPoint(x: 160, y: 50), CGPoint(x: 80, y: 50)],
            geometry: geometry
        )
        #expect(crossing.map(\.event.pressed) == [false, true, true])
        #expect(Set(crossing.map(\.slot)).count == 1)
        // Cancellation ends the contact at the last delivered point, even if
        // the display geometry has disappeared; it synthesizes no new motion.
        let cancelled = contacts.cancel(1)
        #expect(cancelled.map(\.event) == [
            GuestTouchEvent(x: 300, y: 500, pressed: false, cancelled: true)
        ])
        #expect(contacts.cancel(1).isEmpty)
        let ended = contacts.end(
            2, at: CGPoint(x: 130, y: 50), geometry: nil
        )
        #expect(ended.map(\.event) == [
            GuestTouchEvent(x: 700, y: 500, pressed: false)
        ])
        #expect(contacts.cancelAll().isEmpty)
    }

    @Test
    func contactLimitDoesNotAdoptAnIgnoredHeldFinger() throws {
        let geometry = try geometry()
        var contacts = GuestTouchContacts<Int>()
        for id in 0..<10 {
            #expect(contacts.begin(
                id, at: CGPoint(x: 100, y: 50), geometry: geometry
            ).count == 1)
        }
        #expect(contacts.begin(
            10, at: CGPoint(x: 100, y: 50), geometry: geometry
        ).isEmpty)
        _ = contacts.cancel(0)
        #expect(contacts.move(
            10, through: [CGPoint(x: 110, y: 50)], geometry: geometry
        ).isEmpty)
        #expect(contacts.end(
            10, at: CGPoint(x: 110, y: 50), geometry: geometry
        ).isEmpty)
        #expect(contacts.begin(
            11, at: CGPoint(x: 100, y: 50), geometry: geometry
        ).count == 1)
        let cancels = contacts.cancelAll()
        #expect(cancels.count == 10)
        #expect(Set(cancels.map(\.slot)).count == 10)
        #expect(cancels.allSatisfy { !$0.event.pressed && $0.event.cancelled })
    }
}
