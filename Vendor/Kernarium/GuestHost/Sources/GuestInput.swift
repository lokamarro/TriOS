// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import UIKit

/// The coordinates the guest's touchscreen takes, in a space of `size`
/// positions on each axis.
struct GuestInputCoordinateSpace {
    static let maximumDisplayDimension = CGFloat(4095)

    /// The Virtio touchscreen's fixed absolute range, 0 through
    /// `KERNARIUM_RUNTIME_TOUCH_MAXIMUM` on both axes. It does not depend on
    /// the guest's display, so a launch at a small size loses no resolution
    /// at a larger one, and the guest never has to learn the range again
    /// after a mode change.
    static let touchscreen = GuestInputCoordinateSpace(
        size: CGSize(
            width: CGFloat(KERNARIUM_RUNTIME_TOUCH_MAXIMUM) + 1,
            height: CGFloat(KERNARIUM_RUNTIME_TOUCH_MAXIMUM) + 1
        )
    )

    let size: CGSize

    static func boundedDisplaySize(_ size: CGSize) -> CGSize {
        CGSize(
            width: min(size.width, maximumDisplayDimension),
            height: min(size.height, maximumDisplayDimension)
        )
    }

    func location(
        _ point: CGPoint,
        in bounds: CGSize,
        drawableSize: CGSize? = nil,
        contentSize: CGSize? = nil,
        clampToContent: Bool = false
    ) -> (x: UInt32, y: UInt32)? {
        let drawableSize = drawableSize ?? bounds
        let contentSize = contentSize ?? drawableSize
        guard size.width >= 1, size.height >= 1,
              bounds.width > 0, bounds.height > 0,
              point.x.isFinite, point.y.isFinite,
              size.width.isFinite, size.height.isFinite,
              bounds.width.isFinite, bounds.height.isFinite,
              drawableSize.width.isFinite, drawableSize.height.isFinite,
              drawableSize.width > 0, drawableSize.height > 0,
              size.width <= CGFloat(UInt32.max),
              size.height <= CGFloat(UInt32.max) else { return nil }
        guard let viewport = GuestDisplayViewport.aspectFit(
            contentSize: contentSize,
            in: drawableSize
        ) else { return nil }
        let drawablePoint = CGPoint(
            x: point.x * drawableSize.width / bounds.width,
            y: point.y * drawableSize.height / bounds.height
        )
        if !clampToContent, drawablePoint.x < viewport.minX ||
            drawablePoint.x > viewport.maxX ||
            drawablePoint.y < viewport.minY ||
            drawablePoint.y > viewport.maxY {
            return nil
        }
        return coordinate(ofDrawable: drawablePoint, in: viewport)
    }

    /// The position `drawablePoint` has in this space once it is clamped into
    /// `viewport`, the aspect-fit rectangle the guest's display occupies.
    func coordinate(
        ofDrawable drawablePoint: CGPoint,
        in viewport: CGRect
    ) -> (x: UInt32, y: UInt32) {
        let clamped = CGPoint(
            x: min(max(drawablePoint.x, viewport.minX), viewport.maxX),
            y: min(max(drawablePoint.y, viewport.minY), viewport.maxY)
        )
        let x = min(
            max(
                (
                    (clamped.x - viewport.minX) * size.width / viewport.width
                ).rounded(.down),
                0
            ),
            size.width - 1
        )
        let y = min(
            max(
                (
                    (clamped.y - viewport.minY) * size.height / viewport.height
                ).rounded(.down),
                0
            ),
            size.height - 1
        )
        return (UInt32(x), UInt32(y))
    }
}

enum GuestDisplayViewport {
    static func aspectFit(
        contentSize: CGSize,
        in containerSize: CGSize
    ) -> CGRect? {
        guard contentSize.width.isFinite, contentSize.height.isFinite,
              containerSize.width.isFinite, containerSize.height.isFinite,
              contentSize.width > 0, contentSize.height > 0,
              containerSize.width > 0, containerSize.height > 0 else {
            return nil
        }
        let scale = min(
            containerSize.width / contentSize.width,
            containerSize.height / contentSize.height
        )
        let width = contentSize.width * scale
        let height = contentSize.height * scale
        return CGRect(
            x: (containerSize.width - width) / 2,
            y: (containerSize.height - height) / 2,
            width: width,
            height: height
        )
    }
}

/// A contact's state at a point of the guest's touchscreen range. A
/// `cancelled` event ends the contact where it was last reported without
/// lifting it: the guest sees a palm in place of a release, so no tap or
/// click follows. It is never pressed.
struct GuestTouchEvent: Equatable {
    let x: UInt32
    let y: UInt32
    let pressed: Bool
    let cancelled: Bool

    init(x: UInt32, y: UInt32, pressed: Bool, cancelled: Bool = false) {
        self.x = x
        self.y = y
        self.pressed = pressed
        self.cancelled = cancelled
    }
}

struct GuestTouchContactEvent: Equatable {
    let slot: UInt32
    let event: GuestTouchEvent
}

/// Each UIKit contact owns a path and a stable slot, including while it is
/// outside the guest's visible rectangle. Excess fingers stay ignored until
/// they lift, so freeing a slot cannot turn a held finger into a new gesture.
/// A contact UIKit takes away, and every contact when the display mode changes
/// or the view leaves its window, is cancelled rather than released: the guest
/// must not read the end as the user's lift.
struct GuestTouchContacts<Identifier: Hashable> {
    private struct Contact {
        let slot: UInt32
        var path = GuestTouchPath()
    }

    private var contacts: [Identifier: Contact] = [:]

    mutating func begin(
        _ id: Identifier,
        at point: CGPoint,
        geometry: GuestTouchGeometry?
    ) -> [GuestTouchContactEvent] {
        guard contacts[id] == nil,
              let slot = (0..<UInt32(KERNARIUM_RUNTIME_TOUCH_CONTACTS)).first(
                  where: { candidate in
                      !contacts.values.contains { $0.slot == candidate }
                  }
              ) else { return [] }
        var contact = Contact(slot: slot)
        let events = contact.path.begin(at: point, geometry: geometry)
        contacts[id] = contact
        return events.map { GuestTouchContactEvent(slot: slot, event: $0) }
    }

    mutating func move(
        _ id: Identifier,
        through points: [CGPoint],
        geometry: GuestTouchGeometry?
    ) -> [GuestTouchContactEvent] {
        guard var contact = contacts[id] else { return [] }
        let events = contact.path.move(through: points, geometry: geometry)
        contacts[id] = contact
        return events.map { GuestTouchContactEvent(slot: contact.slot, event: $0) }
    }

    mutating func end(
        _ id: Identifier,
        at point: CGPoint,
        geometry: GuestTouchGeometry?
    ) -> [GuestTouchContactEvent] {
        guard var contact = contacts.removeValue(forKey: id) else { return [] }
        return contact.path.end(at: point, geometry: geometry).map {
            GuestTouchContactEvent(slot: contact.slot, event: $0)
        }
    }

    mutating func cancel(_ id: Identifier) -> [GuestTouchContactEvent] {
        guard var contact = contacts.removeValue(forKey: id) else { return [] }
        return contact.path.cancel().map {
            GuestTouchContactEvent(slot: contact.slot, event: $0)
        }
    }

    mutating func cancelAll() -> [GuestTouchContactEvent] {
        let ids = contacts.keys.sorted {
            contacts[$0]!.slot < contacts[$1]!.slot
        }
        return ids.flatMap { cancel($0) }
    }
}

enum GuestTouchSamples {
    static func ordered(
        coalesced: [CGPoint],
        current: CGPoint
    ) -> [CGPoint] {
        guard coalesced.last != current else { return coalesced }
        return coalesced + [current]
    }
}

struct GuestTouchGeometry {
    private let coordinateSpace: GuestInputCoordinateSpace
    private let boundsSize: CGSize
    private let drawableSize: CGSize
    private let viewport: CGRect

    init?(
        coordinateSpace: GuestInputCoordinateSpace,
        boundsSize: CGSize,
        drawableSize: CGSize,
        contentSize: CGSize
    ) {
        guard boundsSize.width.isFinite, boundsSize.height.isFinite,
              boundsSize.width > 0, boundsSize.height > 0,
              drawableSize.width.isFinite, drawableSize.height.isFinite,
              drawableSize.width > 0, drawableSize.height > 0,
              let viewport = GuestDisplayViewport.aspectFit(
                  contentSize: contentSize,
                  in: drawableSize
              ) else { return nil }
        self.coordinateSpace = coordinateSpace
        self.boundsSize = boundsSize
        self.drawableSize = drawableSize
        self.viewport = viewport
    }

    fileprivate func drawablePoint(for point: CGPoint) -> CGPoint? {
        guard point.x.isFinite, point.y.isFinite else { return nil }
        let drawablePoint = CGPoint(
            x: point.x * drawableSize.width / boundsSize.width,
            y: point.y * drawableSize.height / boundsSize.height
        )
        guard drawablePoint.x.isFinite, drawablePoint.y.isFinite else {
            return nil
        }
        return drawablePoint
    }

    fileprivate func contains(_ point: CGPoint) -> Bool {
        point.x >= viewport.minX && point.x <= viewport.maxX &&
            point.y >= viewport.minY && point.y <= viewport.maxY
    }

    fileprivate func touchEvent(
        at drawablePoint: CGPoint,
        pressed: Bool
    ) -> GuestTouchEvent? {
        guard drawablePoint.x.isFinite, drawablePoint.y.isFinite else {
            return nil
        }
        let location = coordinateSpace.coordinate(
            ofDrawable: drawablePoint,
            in: viewport
        )
        return GuestTouchEvent(
            x: location.x,
            y: location.y,
            pressed: pressed
        )
    }

    fileprivate func clippedSegment(
        from start: CGPoint,
        to end: CGPoint
    ) -> (
        entry: CGPoint,
        exit: CGPoint,
        entryParameter: CGFloat,
        exitParameter: CGFloat
    )? {
        let delta = CGPoint(x: end.x - start.x, y: end.y - start.y)
        var entryParameter = CGFloat.zero
        var exitParameter = CGFloat(1)

        func clipped(
            _ direction: CGFloat,
            _ distance: CGFloat,
            entry: inout CGFloat,
            exit: inout CGFloat
        ) -> Bool {
            if direction == 0 {
                return distance >= 0
            }
            let parameter = distance / direction
            if direction < 0 {
                guard parameter <= exit else { return false }
                entry = max(entry, parameter)
            } else {
                guard parameter >= entry else { return false }
                exit = min(exit, parameter)
            }
            return true
        }

        guard clipped(
            -delta.x,
            start.x - viewport.minX,
            entry: &entryParameter,
            exit: &exitParameter
        ), clipped(
            delta.x,
            viewport.maxX - start.x,
            entry: &entryParameter,
            exit: &exitParameter
        ), clipped(
            -delta.y,
            start.y - viewport.minY,
            entry: &entryParameter,
            exit: &exitParameter
        ), clipped(
            delta.y,
            viewport.maxY - start.y,
            entry: &entryParameter,
            exit: &exitParameter
        ), entryParameter <= exitParameter else { return nil }

        return (
            CGPoint(
                x: start.x + delta.x * entryParameter,
                y: start.y + delta.y * entryParameter
            ),
            CGPoint(
                x: start.x + delta.x * exitParameter,
                y: start.y + delta.y * exitParameter
            ),
            entryParameter,
            exitParameter
        )
    }
}

struct GuestTouchPath {
    private var previousPoint: CGPoint?
    private var guestContactActive = false
    private var lastGuestEvent: GuestTouchEvent?

    mutating func begin(
        at point: CGPoint,
        geometry: GuestTouchGeometry?
    ) -> [GuestTouchEvent] {
        previousPoint = point
        guestContactActive = false
        lastGuestEvent = nil
        guard let geometry,
              let drawablePoint = geometry.drawablePoint(for: point),
              geometry.contains(drawablePoint),
              let event = geometry.touchEvent(
                  at: drawablePoint,
                  pressed: true
              ) else { return [] }
        guestContactActive = true
        lastGuestEvent = event
        return [event]
    }

    mutating func move(
        to point: CGPoint,
        geometry: GuestTouchGeometry?
    ) -> [GuestTouchEvent] {
        guard let previousPoint else {
            return begin(at: point, geometry: geometry)
        }
        self.previousPoint = point
        guard let geometry,
              let start = geometry.drawablePoint(for: previousPoint),
              let end = geometry.drawablePoint(for: point) else { return [] }

        var events: [GuestTouchEvent] = []
        let startInside = geometry.contains(start)
        let endInside = geometry.contains(end)

        if guestContactActive && !startInside {
            appendLastRelease(to: &events)
        }

        if guestContactActive {
            if endInside {
                append(
                    geometry.touchEvent(at: end, pressed: true),
                    to: &events
                )
            } else if let segment = geometry.clippedSegment(
                from: start,
                to: end
            ) {
                append(
                    geometry.touchEvent(at: segment.exit, pressed: false),
                    to: &events
                )
            } else {
                appendLastRelease(to: &events)
            }
            return events
        }

        guard let segment = geometry.clippedSegment(from: start, to: end),
              startInside || endInside ||
                segment.exitParameter > segment.entryParameter else {
            return events
        }
        append(
            geometry.touchEvent(at: segment.entry, pressed: true),
            to: &events
        )
        if endInside {
            if segment.entryParameter < 1 {
                append(
                    geometry.touchEvent(at: end, pressed: true),
                    to: &events
                )
            }
        } else {
            append(
                geometry.touchEvent(at: segment.exit, pressed: false),
                to: &events
            )
        }
        return events
    }

    mutating func move(
        through points: [CGPoint],
        geometry: GuestTouchGeometry?
    ) -> [GuestTouchEvent] {
        var events: [GuestTouchEvent] = []
        for point in points {
            events.append(contentsOf: move(to: point, geometry: geometry))
        }
        return events
    }

    mutating func end(
        at point: CGPoint,
        geometry: GuestTouchGeometry?
    ) -> [GuestTouchEvent] {
        var events = move(to: point, geometry: geometry)
        appendLastRelease(to: &events)
        previousPoint = nil
        return events
    }

    /// Withdraws the guest's contact where it was last reported, with no
    /// movement. A path with no contact in the guest, because it began or
    /// has been clipped outside the visible rectangle, has nothing to cancel:
    /// its clipped exit already released it.
    mutating func cancel() -> [GuestTouchEvent] {
        var events: [GuestTouchEvent] = []
        if guestContactActive, let lastGuestEvent {
            events.append(GuestTouchEvent(
                x: lastGuestEvent.x,
                y: lastGuestEvent.y,
                pressed: false,
                cancelled: true
            ))
        }
        guestContactActive = false
        lastGuestEvent = nil
        previousPoint = nil
        return events
    }

    private mutating func append(
        _ event: GuestTouchEvent?,
        to events: inout [GuestTouchEvent]
    ) {
        guard let event else { return }
        events.append(event)
        guestContactActive = event.pressed
        lastGuestEvent = event.pressed ? event : nil
    }

    private mutating func appendLastRelease(
        to events: inout [GuestTouchEvent]
    ) {
        guard guestContactActive, let lastGuestEvent else { return }
        events.append(GuestTouchEvent(
            x: lastGuestEvent.x,
            y: lastGuestEvent.y,
            pressed: false
        ))
        guestContactActive = false
        self.lastGuestEvent = nil
    }
}
