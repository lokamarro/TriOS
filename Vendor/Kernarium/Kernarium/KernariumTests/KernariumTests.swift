// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing
import UIKit
@testable import Kernarium

struct KernariumTests {
    private static let zstdFixture = Data([
        0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x68, 0xf4, 0x00,
        0x00, 0xb0, 0x4b, 0x45, 0x52, 0x4e, 0x41, 0x52,
        0x49, 0x55, 0x4d, 0x2d, 0x5a, 0x53, 0x54, 0x44,
        0x2d, 0x42, 0x45, 0x47, 0x49, 0x4e, 0x0a, 0x00,
        0x01, 0x00, 0xce, 0xff, 0x73, 0xd0, 0x04, 0x02,
        0x00, 0x10, 0x00, 0x02, 0x00, 0x10, 0x00, 0x02,
        0x00, 0x10, 0x00, 0x02, 0x00, 0x10, 0x00, 0x02,
        0x00, 0x10, 0x00, 0x02, 0x00, 0x10, 0x00, 0x02,
        0x00, 0x10, 0x00, 0x02, 0x00, 0x10, 0x00, 0x02,
        0x00, 0x10, 0x00, 0x02, 0x00, 0x10, 0x00, 0x02,
        0x00, 0x10, 0x00, 0x02, 0x00, 0x10, 0x00, 0x02,
        0x00, 0x10, 0x00, 0x02, 0x00, 0x10, 0x00, 0x02,
        0x00, 0x10, 0x00, 0x75, 0x00, 0x00, 0x20, 0x45,
        0x4e, 0x44, 0x0a, 0x02, 0x00, 0x18, 0x00, 0x80,
        0x74, 0xf0, 0x01, 0x20, 0xe7, 0x79, 0xb9, 0xf0,
    ])
    private static let zstdPrefix = Data("KERNARIUM-ZSTD-BEGIN\n".utf8)
    private static let zstdSuffix = Data("KERNARIUM-ZSTD-END\n".utf8)
    private static let zstdOutputSize = 2 * 1024 * 1024 +
        zstdPrefix.count + zstdSuffix.count

    @Test
    func thermalStateReportHasStableMachineReadableFields() {
        #expect(ThermalStateReport.name(for: .nominal) == "nominal")
        #expect(ThermalStateReport.name(for: .fair) == "fair")
        #expect(ThermalStateReport.name(for: .serious) == "serious")
        #expect(ThermalStateReport.name(for: .critical) == "critical")
        #expect(
            ThermalStateReport.line(
                phase: .start,
                state: .nominal,
                uptime: 12.5
            ) == "Kernarium thermal state: phase=start state=nominal " +
                "uptime=12.500"
        )
    }

    @Test
    @MainActor
    func floatingControlsAndSpecialKeysHaveExpectedGeometryAndCodes() {
        #expect(FloatingControlMetrics.buttonSide == 32)
        #expect(GuestViewMode.defaultMode == .framebuffer)
        #expect(UIImage(systemName: "terminal") != nil)
        #expect(UIImage(systemName: "display") != nil)
        #expect(GuestSpecialKey.power.code == 116)
        #expect(GuestSpecialKey.volumeUp.code == 115)
        #expect(GuestSpecialKey.volumeDown.code == 114)
        #expect(GuestSpecialKey.escape.code == 1)
        #expect(GuestSpecialKey.tab.code == 15)
        #expect(GuestSpecialKey.home.code == 102)
        for key in GuestSpecialKey.allCases {
            #expect(UIImage(systemName: key.systemName) != nil)
        }
    }

    @Test
    @MainActor
    func touchCoordinatesStayAlignedWhenTheDisplayViewResizes() throws {
        let coordinates = GuestInputCoordinateSpace(
            size: CGSize(width: 440, height: 860)
        )
        let portrait = try #require(coordinates.location(
            CGPoint(x: 200, y: 400),
            in: CGSize(width: 400, height: 800)
        ))
        let landscape = try #require(coordinates.location(
            CGPoint(x: 500, y: 250),
            in: CGSize(width: 1000, height: 500)
        ))
        #expect(portrait.x == 220)
        #expect(portrait.y == 430)
        #expect(landscape.x == portrait.x)
        #expect(landscape.y == portrait.y)

        let lowerEdge = try #require(coordinates.location(
            CGPoint(x: 1000, y: 500),
            in: CGSize(width: 1000, height: 500)
        ))
        #expect(lowerEdge.x == 439)
        #expect(lowerEdge.y == 859)
        #expect(coordinates.location(
            CGPoint(x: CGFloat.nan, y: 1),
            in: CGSize(width: 1000, height: 500)
        ) == nil)

        let oversized = GuestInputCoordinateSpace.boundedDisplaySize(
            CGSize(width: 6016, height: 3384)
        )
        #expect(oversized.width == 4095)
        #expect(oversized.height == 3384)
    }

    @Test
    @MainActor
    func touchCoordinatesAccountForAspectFitLetterboxing() throws {
        let coordinates = GuestInputCoordinateSpace(
            size: CGSize(width: 400, height: 800)
        )
        let bounds = CGSize(width: 500, height: 250)
        let drawable = CGSize(width: 1000, height: 500)
        let square = CGSize(width: 800, height: 800)
        let viewport = try #require(GuestDisplayViewport.aspectFit(
            contentSize: square,
            in: drawable
        ))
        #expect(viewport == CGRect(x: 250, y: 0, width: 500, height: 500))

        #expect(coordinates.location(
            CGPoint(x: 100, y: 125),
            in: bounds,
            drawableSize: drawable,
            contentSize: square
        ) == nil)

        let center = try #require(coordinates.location(
            CGPoint(x: 250, y: 125),
            in: bounds,
            drawableSize: drawable,
            contentSize: square
        ))
        #expect(center.x == 200)
        #expect(center.y == 400)

        let clampedBar = try #require(coordinates.location(
            CGPoint(x: 100, y: 125),
            in: bounds,
            drawableSize: drawable,
            contentSize: square,
            clampToContent: true
        ))
        #expect(clampedBar.x == 0)
        #expect(clampedBar.y == 400)

        let wideModeCenter = try #require(coordinates.location(
            CGPoint(x: 250, y: 125),
            in: bounds,
            drawableSize: drawable,
            contentSize: CGSize(width: 1600, height: 900)
        ))
        #expect(wideModeCenter.x == center.x)
        #expect(wideModeCenter.y == center.y)
        #expect(GuestDisplayViewport.aspectFit(
            contentSize: .zero,
            in: drawable
        ) == nil)
    }

    @Test
    @MainActor
    func touchscreenRangeIsFixedAndIndependentOfTheViewSize() throws {
        let maximum = UInt32(KERNARIUM_RUNTIME_TOUCH_MAXIMUM)
        #expect(maximum == 32767)
        let coordinates = GuestInputCoordinateSpace.touchscreen
        #expect(coordinates.size == CGSize(width: 32768, height: 32768))

        // The same fraction of any view is the same coordinate, from the
        // launch size to a window grown well past it.
        for bounds in [
            CGSize(width: 440, height: 860),
            CGSize(width: 1133, height: 692),
            CGSize(width: 3000, height: 2000),
        ] {
            let origin = try #require(coordinates.location(
                .zero,
                in: bounds
            ))
            let center = try #require(coordinates.location(
                CGPoint(x: bounds.width / 2, y: bounds.height / 2),
                in: bounds
            ))
            let corner = try #require(coordinates.location(
                CGPoint(x: bounds.width, y: bounds.height),
                in: bounds
            ))
            #expect(origin.x == 0 && origin.y == 0)
            #expect(center.x == 16384 && center.y == 16384)
            #expect(corner.x == maximum && corner.y == maximum)
        }
    }

    @Test
    @MainActor
    func touchResolutionDoesNotCollapseAfterTheViewGrows() throws {
        // A launch-sized range of 440 columns would give a view grown to
        // 3000 points 440 distinct values; the fixed range keeps every point.
        let coordinates = GuestInputCoordinateSpace.touchscreen
        let bounds = CGSize(width: 3000, height: 2000)
        var previous: UInt32?
        for column in 0..<3000 {
            let location = try #require(coordinates.location(
                CGPoint(x: CGFloat(column) + 0.5, y: 1000),
                in: bounds
            ))
            if let previous { #expect(location.x > previous) }
            previous = location.x
        }
    }

    @Test
    @MainActor
    func touchPathReportsFullRangeCoordinates() throws {
        let geometry = try #require(GuestTouchGeometry(
            coordinateSpace: .touchscreen,
            boundsSize: CGSize(width: 500, height: 250),
            drawableSize: CGSize(width: 1000, height: 500),
            contentSize: CGSize(width: 800, height: 800)
        ))
        var path = GuestTouchPath()

        // The guest square spans bounds x 125...375 (drawable 250...750).
        #expect(path.begin(
            at: CGPoint(x: 250, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 16384, y: 16384, pressed: true),
        ])
        #expect(path.move(
            to: CGPoint(x: 200, y: 62.5),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 9830, y: 8192, pressed: true),
        ])
        // The path leaves through the right edge of the guest square, at the
        // top of the range's last column.
        #expect(path.end(
            at: CGPoint(x: 450, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 32767, y: 13926, pressed: false),
        ])
    }

    @Test
    @MainActor
    func touchPathStartsAndEndsAtGuestDisplayEdges() throws {
        let geometry = try #require(GuestTouchGeometry(
            coordinateSpace: GuestInputCoordinateSpace(
                size: CGSize(width: 400, height: 800)
            ),
            boundsSize: CGSize(width: 500, height: 250),
            drawableSize: CGSize(width: 1000, height: 500),
            contentSize: CGSize(width: 800, height: 800)
        ))
        var path = GuestTouchPath()

        #expect(path.begin(
            at: CGPoint(x: 50, y: 125),
            geometry: geometry
        ).isEmpty)
        #expect(path.move(
            to: CGPoint(x: 200, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 0, y: 400, pressed: true),
            GuestTouchEvent(x: 120, y: 400, pressed: true),
        ])
        #expect(path.move(
            to: CGPoint(x: 450, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 399, y: 400, pressed: false),
        ])
        #expect(path.move(
            to: CGPoint(x: 300, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 399, y: 400, pressed: true),
            GuestTouchEvent(x: 280, y: 400, pressed: true),
        ])
        #expect(path.end(
            at: CGPoint(x: 50, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 0, y: 400, pressed: false),
        ])
    }

    @Test
    @MainActor
    func coalescedTouchSegmentsCrossTheGuestDisplay() throws {
        let geometry = try #require(GuestTouchGeometry(
            coordinateSpace: GuestInputCoordinateSpace(
                size: CGSize(width: 400, height: 800)
            ),
            boundsSize: CGSize(width: 500, height: 250),
            drawableSize: CGSize(width: 1000, height: 500),
            contentSize: CGSize(width: 800, height: 800)
        ))
        var path = GuestTouchPath()

        #expect(path.begin(
            at: CGPoint(x: 50, y: 125),
            geometry: geometry
        ).isEmpty)
        #expect(path.move(
            to: CGPoint(x: 450, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 0, y: 400, pressed: true),
            GuestTouchEvent(x: 399, y: 400, pressed: false),
        ])
        #expect(path.end(
            at: CGPoint(x: 50, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 399, y: 400, pressed: true),
            GuestTouchEvent(x: 0, y: 400, pressed: false),
        ])
    }

    @Test
    @MainActor
    func coalescedCurvedTouchPathPreservesViewportCrossings() throws {
        let geometry = try #require(GuestTouchGeometry(
            coordinateSpace: GuestInputCoordinateSpace(
                size: CGSize(width: 400, height: 800)
            ),
            boundsSize: CGSize(width: 500, height: 250),
            drawableSize: CGSize(width: 1000, height: 500),
            contentSize: CGSize(width: 800, height: 800)
        ))
        let start = CGPoint(x: 50, y: 50)
        let firstInterior = CGPoint(x: 200, y: 75)
        let secondInterior = CGPoint(x: 300, y: 175)
        let current = CGPoint(x: 50, y: 200)

        var endpointOnlyPath = GuestTouchPath()
        #expect(endpointOnlyPath.begin(
            at: start,
            geometry: geometry
        ).isEmpty)
        #expect(endpointOnlyPath.move(
            to: current,
            geometry: geometry
        ).isEmpty)

        let samples = GuestTouchSamples.ordered(
            coalesced: [firstInterior, secondInterior, current],
            current: current
        )
        #expect(samples == [firstInterior, secondInterior, current])
        #expect(GuestTouchSamples.ordered(
            coalesced: [firstInterior, secondInterior],
            current: current
        ) == [firstInterior, secondInterior, current])

        var coalescedPath = GuestTouchPath()
        #expect(coalescedPath.begin(
            at: start,
            geometry: geometry
        ).isEmpty)
        #expect(coalescedPath.move(
            through: samples,
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 0, y: 200, pressed: true),
            GuestTouchEvent(x: 120, y: 240, pressed: true),
            GuestTouchEvent(x: 280, y: 560, pressed: true),
            GuestTouchEvent(x: 0, y: 616, pressed: false),
        ])
    }

    @Test
    @MainActor
    func endingTouchProcessesItsFinalViewportCrossing() throws {
        let geometry = try #require(GuestTouchGeometry(
            coordinateSpace: GuestInputCoordinateSpace(
                size: CGSize(width: 400, height: 800)
            ),
            boundsSize: CGSize(width: 500, height: 250),
            drawableSize: CGSize(width: 1000, height: 500),
            contentSize: CGSize(width: 800, height: 800)
        ))
        var path = GuestTouchPath()

        #expect(path.begin(
            at: CGPoint(x: 200, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 120, y: 400, pressed: true),
        ])
        #expect(path.end(
            at: CGPoint(x: 450, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 399, y: 400, pressed: false),
        ])

        #expect(path.begin(
            at: CGPoint(x: 50, y: 125),
            geometry: geometry
        ).isEmpty)
        #expect(path.end(
            at: CGPoint(x: 200, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 0, y: 400, pressed: true),
            GuestTouchEvent(x: 120, y: 400, pressed: true),
            GuestTouchEvent(x: 120, y: 400, pressed: false),
        ])
    }

    /// The guest square spans bounds x 125...375 of a 500 x 250 view, and
    /// maps onto a 400 x 800 touchscreen.
    @MainActor
    private func cancelGeometry() throws -> GuestTouchGeometry {
        try #require(GuestTouchGeometry(
            coordinateSpace: GuestInputCoordinateSpace(
                size: CGSize(width: 400, height: 800)
            ),
            boundsSize: CGSize(width: 500, height: 250),
            drawableSize: CGSize(width: 1000, height: 500),
            contentSize: CGSize(width: 800, height: 800)
        ))
    }

    @Test
    @MainActor
    func cancellingATouchEndsItWhereItWasAndMovesNothing() throws {
        let geometry = try cancelGeometry()
        var path = GuestTouchPath()

        #expect(path.begin(
            at: CGPoint(x: 200, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 120, y: 400, pressed: true),
        ])
        #expect(path.move(
            to: CGPoint(x: 250, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 200, y: 400, pressed: true),
        ])
        // The cancel states the last point the guest was given, and the
        // contact is no longer there: a second one has nothing to cancel.
        #expect(path.cancel() == [
            GuestTouchEvent(x: 200, y: 400, pressed: false, cancelled: true),
        ])
        #expect(path.cancel().isEmpty)
        // UIKit may still report the cancelled touch; the path ignores it
        // until a new touch begins, which starts a contact like any other.
        #expect(path.begin(
            at: CGPoint(x: 200, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 120, y: 400, pressed: true),
        ])
        // It ends as a release, with the final position before it.
        #expect(path.end(
            at: CGPoint(x: 200, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 120, y: 400, pressed: true),
            GuestTouchEvent(x: 120, y: 400, pressed: false),
        ])
    }

    @Test
    @MainActor
    func aTouchOffTheDisplayHasNothingToCancel() throws {
        let geometry = try cancelGeometry()
        var path = GuestTouchPath()

        // It began outside the guest's square: the guest has no contact.
        #expect(path.begin(
            at: CGPoint(x: 50, y: 125),
            geometry: geometry
        ).isEmpty)
        #expect(path.cancel().isEmpty)

        // It left the square: leaving released it, as a real release, and
        // the cancel that follows has nothing left to withdraw.
        #expect(path.begin(
            at: CGPoint(x: 200, y: 125),
            geometry: geometry
        ) == [
            GuestTouchEvent(x: 120, y: 400, pressed: true),
        ])
        let leaving = path.move(
            to: CGPoint(x: 450, y: 125),
            geometry: geometry
        )
        #expect(leaving == [
            GuestTouchEvent(x: 399, y: 400, pressed: false),
        ])
        #expect(path.cancel().isEmpty)
    }

    @Test
    @MainActor
    func endingAndClippingAContactAreReleasesNotCancels() throws {
        let geometry = try cancelGeometry()
        var path = GuestTouchPath()
        var events = path.begin(
            at: CGPoint(x: 200, y: 125),
            geometry: geometry
        )
        // Out of the square and back in, and the finger lifts there.
        events += path.move(
            through: [
                CGPoint(x: 450, y: 125),
                CGPoint(x: 300, y: 125),
                CGPoint(x: 450, y: 125),
            ],
            geometry: geometry
        )
        events += path.end(at: CGPoint(x: 300, y: 125), geometry: geometry)
        #expect(events.map(\.pressed) ==
            [true, false, true, true, false, true, true, false])
        #expect(events.allSatisfy { !$0.cancelled })
        #expect(path.cancel().isEmpty)
    }

    @Test
    @MainActor
    func cancellingContactsCancelsEveryOneInItsSlot() throws {
        let geometry = try cancelGeometry()
        var contacts = GuestTouchContacts<Int>()
        let first = try #require(contacts.begin(
            1, at: CGPoint(x: 200, y: 125), geometry: geometry
        ).first)
        let second = try #require(contacts.begin(
            2, at: CGPoint(x: 300, y: 125), geometry: geometry
        ).first)
        // A finger that began outside the guest's square holds a slot and
        // gives the guest nothing, to start or to cancel.
        #expect(contacts.begin(
            3, at: CGPoint(x: 50, y: 125), geometry: geometry
        ).isEmpty)
        #expect(first.slot != second.slot)

        #expect(contacts.cancel(2) == [GuestTouchContactEvent(
            slot: second.slot,
            event: GuestTouchEvent(
                x: 280, y: 400, pressed: false, cancelled: true
            )
        )])
        // A cancelled finger is gone: UIKit's later phases for it do nothing.
        #expect(contacts.move(
            2, through: [CGPoint(x: 310, y: 125)], geometry: geometry
        ).isEmpty)
        #expect(contacts.end(
            2, at: CGPoint(x: 310, y: 125), geometry: geometry
        ).isEmpty)
        #expect(contacts.cancel(2).isEmpty)

        // The display mode changes: what the guest still holds is cancelled,
        // in slot order, and the finger outside has nothing to cancel.
        #expect(contacts.cancelAll() == [GuestTouchContactEvent(
            slot: first.slot,
            event: GuestTouchEvent(
                x: 120, y: 400, pressed: false, cancelled: true
            )
        )])
        #expect(contacts.cancelAll().isEmpty)

        // The slots are free again.
        let third = try #require(contacts.begin(
            4, at: CGPoint(x: 200, y: 125), geometry: geometry
        ).first)
        #expect(third.slot == first.slot)
        #expect(contacts.end(
            4, at: CGPoint(x: 200, y: 125), geometry: geometry
        ).map(\.event) == [
            GuestTouchEvent(x: 120, y: 400, pressed: true),
            GuestTouchEvent(x: 120, y: 400, pressed: false),
        ])
    }

    @Test
    @MainActor
    func theApplicationHasOneSceneBecauseItRunsOneGuest() throws {
        // The runtime boots one guest per process, and closing a window
        // stops it. With a second window, closing that one would end the
        // guest the first still shows, so iPadOS must not offer one.
        let manifest = try #require(
            Bundle.main.object(
                forInfoDictionaryKey: "UIApplicationSceneManifest"
            ) as? [String: Any]
        )
        #expect(manifest["UIApplicationSupportsMultipleScenes"] as? Bool == false)
    }

    @Test
    @MainActor
    func aTransitionHoldsTheGuestModeUntilItCompletes() {
        var hold = GuestResizeHold()

        let rotation = hold.transitionWillBegin()
        #expect(rotation.action == .begin)
        #expect(hold.transitionDidComplete(token: rotation.token) == .finish)
    }

    @Test
    @MainActor
    func aSupersededTransitionNeverFinishesTheHold() {
        var hold = GuestResizeHold()

        let first = hold.transitionWillBegin()
        let second = hold.transitionWillBegin()
        #expect(hold.transitionDidComplete(token: first.token) == .none)
        #expect(hold.transitionDidComplete(token: second.token) == .finish)
    }

    @Test
    @MainActor
    func anInteractiveWindowResizeHoldsTheModeThroughEveryTransition() {
        var hold = GuestResizeHold()

        #expect(hold.interactiveResizeChanged(true) == .begin)
        // Each size the drag passes through runs a transition. None of them
        // lets the guest follow that size.
        for _ in 0..<4 {
            let step = hold.transitionWillBegin()
            #expect(hold.transitionDidComplete(token: step.token) == .none)
        }
        #expect(hold.interactiveResizeChanged(false) == .finish)

        // The next transition is an ordinary one again.
        let rotation = hold.transitionWillBegin()
        #expect(hold.transitionDidComplete(token: rotation.token) == .finish)
    }

    @Test
    @MainActor
    func theEndOfAGestureWaitsForItsLastTransition() {
        var hold = GuestResizeHold()

        #expect(hold.interactiveResizeChanged(true) == .begin)
        let last = hold.transitionWillBegin()
        #expect(hold.interactiveResizeChanged(false) == .none)
        #expect(hold.transitionDidComplete(token: last.token) == .finish)
    }

    @Test
    @MainActor
    func repeatedGestureStatesRaiseOneBeginAndOneFinish() {
        var hold = GuestResizeHold()

        #expect(hold.interactiveResizeChanged(false) == .none)
        #expect(hold.interactiveResizeChanged(true) == .begin)
        #expect(hold.interactiveResizeChanged(true) == .none)
        #expect(hold.interactiveResizeChanged(false) == .finish)
        #expect(hold.interactiveResizeChanged(false) == .none)
    }

    @Test
    @MainActor
    func overlayDiskIsRecreatedAtRequestedSize() throws {
        let manager = FileManager.default
        let directory = try makeScratchDirectory("overlay-disk")
        defer { try? manager.removeItem(at: directory) }

        let first = try OverlayDisk.recreate(sizeGiB: 3, in: directory)
        let writer = try FileHandle(forWritingTo: first)
        try writer.write(contentsOf: Data([0xa5]))
        try writer.close()

        let second = try OverlayDisk.recreate(sizeGiB: 3, in: directory)
        let attributes = try manager.attributesOfItem(atPath: second.path)
        let size = attributes[.size] as? NSNumber
        #expect(first == second)
        #expect(size?.uint64Value == 3 * 1024 * 1024 * 1024)

        let reader = try FileHandle(forReadingFrom: second)
        let prefix = try reader.read(upToCount: 1)
        try reader.close()
        #expect(prefix == Data([0]))
    }

    @Test
    func androidBundleSelectionDoesNotRequireReleaseMetadata() throws {
        let manager = FileManager.default
        let directory = try makeScratchDirectory(
            "bundle-selection",
            pathExtension: GuestBundle.pathExtension
        )
        defer { try? manager.removeItem(at: directory) }
        for relative in [
            GuestBundle.Android.systemDisk,
            GuestBundle.Android.layout,
            GuestBundle.Android.commandLine,
        ] {
            let file = directory.appendingPathComponent(relative)
            try manager.createDirectory(
                at: file.deletingLastPathComponent(),
                withIntermediateDirectories: true
            )
            #expect(manager.createFile(atPath: file.path, contents: Data()))
        }

        let bundle = GuestBundle.load(at: directory, fileManager: manager)

        #expect(bundle?.systemDisk.lastPathComponent == "system.img")
        #expect(bundle?.dataDisk.lastPathComponent == "data.img")
        #expect(bundle?.flavor == .android)

        // The data disk is the runtime's to create; a release missing a
        // required file is no bundle.
        try manager.removeItem(at: directory.appendingPathComponent(
            GuestBundle.Android.layout
        ))
        #expect(GuestBundle.load(at: directory, fileManager: manager) == nil)
    }

    @Test
    func configurationDefaultsUseMaximumCPUAndRequestedDefaults() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }

        let configuration = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6,
            guestMemoryBound: Self.unboundedGuestMemory
        )

        #expect(configuration.vCPUCount == 6)
        // The runtime's own default at the loaded vCPU count: what the system
        // leaves a device, and 4096 on a Mac, which sets no limit.
        #expect(
            configuration.guestMemoryMiB ==
                Int(kernarium_runtime_loader_default_guest_memory_mib(
                    6, kernarium_runtime_loader_launch_memory_set()
                ))
        )
        #expect(configuration.overlaySizeGiB == 8)
        #expect(configuration.androidDiskSizeGiB == 8)
        #expect(configuration.rootfsFilename == "kde-20260929-r2.erofs")
        #expect(configuration.retinaMode == false)
        #expect(configuration.autoResizeGuest == true)
        #expect(configuration.androidBootMode == .automatic)
    }

    /// Every physical core is the default and the most the settings, a stored
    /// count and `KERNARIUM_VCPU_COUNT` allow, at most the eight the memory
    /// budget is measured for. The loader answers the rule.
    @Test
    func theVCPULimitIsThePhysicalCoresAtMostEight() {
        #expect(kernarium_runtime_loader_application_vcpu_limit(0) == 1)
        #expect(kernarium_runtime_loader_application_vcpu_limit(UInt32.max) == 8)

        for (physical, limit) in [
            (1, 1), (4, 4), (6, 6), (8, 8), (10, 8), (12, 8), (20, 8),
        ] {
            let (defaults, suiteName) = isolatedDefaults()
            defer { defaults.removePersistentDomain(forName: suiteName) }
            func load(
                _ environment: [String: String] = [:]
            ) -> EmulatorConfiguration {
                EmulatorConfiguration.load(
                    defaults: defaults,
                    physicalProcessorCount: physical,
                    environment: environment,
                    guestMemoryBound: Self.unboundedGuestMemory
                )
            }

            #expect(
                EmulatorConfiguration.maximumVCPUCount(
                    physicalProcessorCount: physical
                ) == limit
            )
            // Nothing stored: every core, at most eight.
            #expect(load().vCPUCount == limit)

            // A stored count above the limit comes down to it; one within
            // it is kept.
            defaults.set(10, forKey: "emulator.vCPUCount")
            #expect(load().vCPUCount == limit)
            defaults.set(limit, forKey: "emulator.vCPUCount")
            #expect(load().vCPUCount == limit)
            defaults.set(1, forKey: "emulator.vCPUCount")
            #expect(load().vCPUCount == 1)

            // The environment states a count from one through the limit
            // and nothing outside it.
            let key = EmulatorConfiguration.vCPUEnvironmentKey
            #expect(load([key: String(limit)]).vCPUCount == limit)
            #expect(load([key: "1"]).vCPUCount == 1)
            #expect(load([key: String(limit + 1)]).vCPUCount == 1)
            // Ten is no longer offered, whatever the device has.
            #expect(load([key: "10"]).vCPUCount == 1)
            #expect(load([key: "0"]).vCPUCount == 1)
            #expect(load([key: "many"]).vCPUCount == 1)
        }
    }

    /// The default is the runtime's rule, asked of the loader with the
    /// conservative launch set: an iPad mini 6's 2,867 MiB gives 1536 MiB at
    /// every vCPU count, larger limits give more in half-gibibyte steps, and a
    /// Mac, which sets no limit, keeps the top of the range.
    @Test
    func theDefaultGuestMemoryIsTheSharedRuleAtTheVCPUCount() {
        let launch = kernarium_runtime_loader_launch_memory_set()
        func defaults(limitMiB: UInt64) -> [Int] {
            (1...8).map { vCPUs in
                Int(kernarium_runtime_loader_default_guest_memory_mib_for_limit(
                    limitMiB << 20, UInt32(vCPUs), launch
                ))
            }
        }
        #expect(defaults(limitMiB: 2867) == Array(repeating: 1536, count: 8))
        #expect(defaults(limitMiB: 3584) == Array(repeating: 2048, count: 8))
        #expect(defaults(limitMiB: 4096) == Array(repeating: 2560, count: 8))
        #expect(defaults(limitMiB: 5120) == Array(repeating: 3584, count: 8))
        #expect(defaults(limitMiB: 6144) == Array(repeating: 4096, count: 8))
        #expect(defaults(limitMiB: 0) == Array(repeating: 4096, count: 8))
        // A limit under the range's bottom keeps the default on the
        // maximum, which is on the stepper's steps.
        let tight = defaults(limitMiB: 2048)
        #expect(tight == [1472, 1472, 1408, 1408, 1344, 1344, 1344, 1344])
        for vCPUs in 1...8 {
            #expect(EmulatorConfiguration.systemDefaultGuestMemory(vCPUs) ==
                Int(kernarium_runtime_loader_default_guest_memory_mib(
                    UInt32(vCPUs), launch
                )))
        }
        // Zero vCPUs count as one.
        #expect(EmulatorConfiguration.systemDefaultGuestMemory(0) ==
            EmulatorConfiguration.systemDefaultGuestMemory(1))
        #expect(
            EmulatorConfiguration.guestMemoryRangeMiB.contains(
                EmulatorConfiguration.systemDefaultGuestMemory(6)
            )
        )
        #expect(
            EmulatorConfiguration.systemDefaultGuestMemory(6) %
                EmulatorConfiguration.guestMemoryStepMiB == 0
        )
    }

    /// A configuration with nothing stored takes the default of the vCPU
    /// count it loads with, whether the count was defaulted, stored or stated
    /// by the environment.
    @Test
    func theDefaultGuestMemoryFollowsTheLoadedVCPUCount() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }
        var asked: [Int] = []
        let rule: EmulatorConfiguration.DefaultGuestMemory = { vCPUs in
            asked.append(vCPUs)
            return 2048 - vCPUs * 64
        }
        func load(_ environment: [String: String] = [:]) -> EmulatorConfiguration {
            asked = []
            return EmulatorConfiguration.load(
                defaults: defaults,
                physicalProcessorCount: 6,
                environment: environment,
                guestMemoryBound: Self.unboundedGuestMemory,
                defaultGuestMemory: rule
            )
        }
        #expect(load().guestMemoryMiB == 2048 - 6 * 64)
        #expect(asked == [6])
        #expect(
            load([EmulatorConfiguration.vCPUEnvironmentKey: "3"])
                .guestMemoryMiB == 2048 - 3 * 64
        )
        #expect(asked == [3])
        defaults.set(2, forKey: "emulator.vCPUCount")
        #expect(load().guestMemoryMiB == 2048 - 2 * 64)
        #expect(asked == [2])
        // A stored size is kept, and the default is not what decides it.
        defaults.set(1024, forKey: "emulator.guestMemoryMiB")
        #expect(load().guestMemoryMiB == 1024)
    }

    @Test
    func aStoredGuestMemoryOutlivesTheDefault() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }
        defaults.set(2048, forKey: "emulator.guestMemoryMiB")

        #expect(
            EmulatorConfiguration.load(
                defaults: defaults,
                physicalProcessorCount: 6,
                guestMemoryBound: Self.unboundedGuestMemory
            ).guestMemoryMiB == 2048
        )
    }

    @Test
    func configurationPersistsAndNormalizesStoredValues() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }

        EmulatorConfiguration(
            vCPUCount: 4,
            guestMemoryMiB: 2048,
            overlaySizeGiB: 16,
            androidDiskSizeGiB: 32,
            rootfsFilename: "phosh.erofs",
            retinaMode: true,
            autoResizeGuest: false,
            androidBootMode: .recovery
        ).save(defaults: defaults)
        #expect(
            EmulatorConfiguration.load(
                defaults: defaults,
                physicalProcessorCount: 6,
                guestMemoryBound: Self.unboundedGuestMemory
            ) == EmulatorConfiguration(
                vCPUCount: 4,
                guestMemoryMiB: 2048,
                overlaySizeGiB: 16,
                androidDiskSizeGiB: 32,
                rootfsFilename: "phosh.erofs",
                    retinaMode: true,
                autoResizeGuest: false,
                androidBootMode: .recovery
            )
        )

        defaults.set(99, forKey: "emulator.vCPUCount")
        defaults.set(1000, forKey: "emulator.guestMemoryMiB")
        defaults.set(0, forKey: "emulator.overlaySizeGiB")
        defaults.set(128, forKey: "emulator.androidDiskSizeGiB")
        defaults.set("../phosh.erofs", forKey: "emulator.rootfsFilename")
        let normalized = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6,
            guestMemoryBound: Self.unboundedGuestMemory
        )
        #expect(normalized.vCPUCount == 6)
        #expect(normalized.guestMemoryMiB == 1024)
        #expect(normalized.overlaySizeGiB == 1)
        #expect(normalized.androidDiskSizeGiB == 64)
        #expect(normalized.rootfsFilename == "kde-20260929-r2.erofs")
        #expect(normalized.retinaMode == true)
        #expect(normalized.autoResizeGuest == false)
        #expect(normalized.androidBootMode == .recovery)
    }

    @Test
    func androidDiskSizeRangeIsSixThroughSixtyFourGiB() {
        #expect(EmulatorConfiguration.androidDiskSizeRangeGiB == 6...64)
        #expect(EmulatorConfiguration.defaultAndroidDiskSizeGiB == 8)

        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }
        // A disk stored below the range, as an older version allowed, is
        // raised to its floor.
        defaults.set(4, forKey: "emulator.androidDiskSizeGiB")
        #expect(
            EmulatorConfiguration.load(
                defaults: defaults,
                physicalProcessorCount: 6
            ).androidDiskSizeGiB == 6
        )

        // The environment override takes a size inside the range only; any
        // other leaves the stored value.
        defaults.set(12, forKey: "emulator.androidDiskSizeGiB")
        for (value, expected) in [("5", 12), ("6", 6), ("64", 64), ("65", 12)] {
            #expect(
                EmulatorConfiguration.load(
                    defaults: defaults,
                    physicalProcessorCount: 6,
                    environment: [
                        EmulatorConfiguration.androidDiskSizeEnvironmentKey:
                            value
                    ]
                ).androidDiskSizeGiB == expected
            )
        }
    }

    @Test
    func gpuBudgetParametersComeFromTheEnvironmentAndDefaultToTheLaunchSet() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }

        // A launch states the conservative launch set unless the environment
        // names a value: A 64, E_rest 320, S 45 + 40 + 64 MiB.
        let plain = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6
        )
        #expect(plain.gpuBudgetApplicationMiB == 64)
        #expect(plain.gpuBudgetEngineMiB == 320)
        #expect(plain.gpuBudgetHeadroomMiB == 149)

        let stated = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6,
            environment: [
                EmulatorConfiguration.gpuBudgetApplicationEnvironmentKey:
                    "300",
                EmulatorConfiguration.gpuBudgetEngineEnvironmentKey: "50",
                EmulatorConfiguration.gpuBudgetHeadroomEnvironmentKey: "100",
            ]
        )
        #expect(stated.gpuBudgetApplicationMiB == 300)
        #expect(stated.gpuBudgetEngineMiB == 50)
        #expect(stated.gpuBudgetHeadroomMiB == 100)

        // A value that is not a whole number in range states nothing, and
        // the launch set stays.
        let refused = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6,
            environment: [
                EmulatorConfiguration.gpuBudgetApplicationEnvironmentKey:
                    "-1",
                EmulatorConfiguration.gpuBudgetEngineEnvironmentKey: "lots",
                EmulatorConfiguration.gpuBudgetHeadroomEnvironmentKey:
                    "99999999",
            ]
        )
        #expect(refused.gpuBudgetApplicationMiB == 64)
        #expect(refused.gpuBudgetEngineMiB == 320)
        #expect(refused.gpuBudgetHeadroomMiB == 149)

        // The parameters are not stored: no setting writes them.
        stated.save(defaults: defaults)
        #expect(
            EmulatorConfiguration.load(
                defaults: defaults,
                physicalProcessorCount: 6
            ).gpuBudgetApplicationMiB == 64
        )
    }

    /// No bound at all: a host that sets no memory limit.
    private static let unboundedGuestMemory: EmulatorConfiguration
        .GuestMemoryBound = { _ in 4096 }

    /// The iPad mini 6's 2,867 MiB with the maximum set (A, E_rest and S of
    /// zero), the rows the runtime's own formula gives at 1 through 6 vCPUs.
    private static let mainGuestMemoryRows = [
        1: 2304, 2: 2304, 3: 2240, 4: 2240, 5: 2176, 6: 2176,
    ]
    private static let mainGuestMemoryBound: EmulatorConfiguration
        .GuestMemoryBound = { vCPUCount in
        mainGuestMemoryRows[vCPUCount] ?? 2176
    }

    @Test
    func theLoaderGivesTheRuntimesGuestMemoryMaximumPerVCPUCount() {
        func maximums(_ set: kernarium_launch_memory_set) -> [Int] {
            (1...8).map { vCPUs in
                Int(kernarium_runtime_loader_maximum_guest_memory_mib_for_limit(
                    2867 << 20, UInt32(vCPUs), set
                ))
            }
        }
        // The maximum set counts none of A, E_rest and S: the most a person
        // may choose. 2048 MiB stays selectable on an iPad mini 6.
        let none = kernarium_launch_memory_set(
            application_mib: 0, engine_mib: 0, headroom_mib: 0
        )
        let maximumSet = kernarium_runtime_loader_maximum_memory_set()
        #expect(maximumSet.application_mib == 0)
        #expect(maximumSet.engine_mib == 0)
        #expect(maximumSet.headroom_mib == 0)
        #expect(maximums(none) == [2304, 2304, 2240, 2240, 2176, 2176, 2176, 2176])
        #expect(maximums(maximumSet) == maximums(none))
        // The conservative launch set the default and the host GPU's budget
        // use: A 64, E_rest 320, and S of 45 + 40 + 64.
        let launch = kernarium_runtime_loader_launch_memory_set()
        #expect(launch.application_mib == 64)
        #expect(launch.engine_mib == 320)
        #expect(launch.headroom_mib == 149)
        #expect(maximums(launch) == [1792, 1728, 1728, 1664, 1664, 1664, 1600, 1600])
        // Other sets keep the formula's shape.
        #expect(
            maximums(kernarium_launch_memory_set(
                application_mib: 56, engine_mib: 563, headroom_mib: 64
            )) == [1664, 1600, 1536, 1536, 1536, 1536, 1472, 1472]
        )
        #expect(
            maximums(kernarium_launch_memory_set(
                application_mib: 64, engine_mib: 604, headroom_mib: 128
            )) == [1536, 1472, 1472, 1408, 1408, 1408, 1344, 1344]
        )
        #expect(
            maximums(kernarium_launch_memory_set(
                application_mib: 80, engine_mib: 644, headroom_mib: 192
            )) == [1408, 1344, 1344, 1280, 1280, 1280, 1216, 1216]
        )
        // No limit: the whole range, whatever the vCPU count.
        for vCPUs: UInt32 in [0, 1, 6, 8, 64] {
            #expect(
                kernarium_runtime_loader_maximum_guest_memory_mib_for_limit(
                    0, vCPUs, none
                ) == 4096
            )
        }
        // The launch set the loader reports is the one the GPU budget
        // defaults follow.
        #expect(
            Int(launch.application_mib) ==
                EmulatorConfiguration.defaultGPUBudgetApplicationMiB
        )
        #expect(
            Int(launch.engine_mib) ==
                EmulatorConfiguration.defaultGPUBudgetEngineMiB
        )
        #expect(
            Int(launch.headroom_mib) ==
                EmulatorConfiguration.defaultGPUBudgetHeadroomMiB
        )
        // The system's own answer is some size of the stepper's range.
        var configuration = EmulatorConfiguration(
            vCPUCount: 4, guestMemoryMiB: 1024, overlaySizeGiB: 8,
            androidDiskSizeGiB: 8, rootfsFilename: "x.erofs",
            retinaMode: false, autoResizeGuest: true, androidBootMode: .automatic
        )
        let system = configuration.maximumGuestMemoryMiB(vCPUCount: 4)
        #expect(EmulatorConfiguration.guestMemoryRangeMiB.contains(system))
        #expect(
            (system - EmulatorConfiguration.guestMemoryRangeMiB.lowerBound) %
                EmulatorConfiguration.guestMemoryStepMiB == 0
        )
        // It is the maximum set's answer for this process, whatever launch
        // parameters the configuration states for the host GPU's budget.
        #expect(
            system == Int(kernarium_runtime_loader_maximum_guest_memory_mib(
                4, maximumSet
            ))
        )
        configuration.gpuBudgetApplicationMiB = 900
        configuration.gpuBudgetEngineMiB = 900
        configuration.gpuBudgetHeadroomMiB = 900
        #expect(configuration.maximumGuestMemoryMiB(vCPUCount: 4) == system)
    }

    @Test
    func aStoredOrDefaultGuestMemoryAboveTheMaximumComesDownOnLoad() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }

        // Nothing stored: the default, or the maximum where that is less.
        let defaultMiB = 1536
        for (bound, expected) in [(4096, defaultMiB), (1344, 1344), (512, 512)] {
            #expect(
                EmulatorConfiguration.load(
                    defaults: defaults,
                    physicalProcessorCount: 6,
                    guestMemoryBound: { _ in bound },
                    defaultGuestMemory: { _ in defaultMiB }
                ).guestMemoryMiB == min(defaultMiB, expected)
            )
        }

        // A stored size is taken down to the maximum of the loaded count and
        // kept below it, whatever the count.
        defaults.set(2304, forKey: "emulator.guestMemoryMiB")
        for (vCPUs, expected) in [
            (1, 2304), (2, 2304), (3, 2240), (4, 2240), (5, 2176), (6, 2176),
        ] {
            defaults.set(vCPUs, forKey: "emulator.vCPUCount")
            #expect(
                EmulatorConfiguration.load(
                    defaults: defaults,
                    physicalProcessorCount: 6,
                    guestMemoryBound: Self.mainGuestMemoryBound
                ).guestMemoryMiB == expected
            )
        }
        defaults.set(1792, forKey: "emulator.guestMemoryMiB")
        #expect(
            EmulatorConfiguration.load(
                defaults: defaults,
                physicalProcessorCount: 6,
                guestMemoryBound: Self.mainGuestMemoryBound
            ).guestMemoryMiB == 1792
        )

        // The bound sees the vCPU count the environment states and nothing
        // else: the launch parameters the environment states change the host
        // GPU's budget, not the most guest RAM the settings offer.
        var seen: [Int] = []
        let loaded = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6,
            environment: [
                EmulatorConfiguration.vCPUEnvironmentKey: "3",
                EmulatorConfiguration.gpuBudgetApplicationEnvironmentKey: "60",
                EmulatorConfiguration.gpuBudgetEngineEnvironmentKey: "600",
                EmulatorConfiguration.gpuBudgetHeadroomEnvironmentKey: "100",
            ],
            guestMemoryBound: { vCPUs in
                seen.append(vCPUs)
                return 1024
            }
        )
        #expect(seen == [3])
        #expect(loaded.gpuBudgetApplicationMiB == 60)
        #expect(loaded.guestMemoryMiB == 1024)
        #expect(
            EmulatorConfiguration.load(
                defaults: defaults,
                physicalProcessorCount: 6,
                environment: [
                    EmulatorConfiguration.guestMemoryEnvironmentKey: "4096"
                ],
                guestMemoryBound: Self.mainGuestMemoryBound
            ).guestMemoryMiB == 4096
        )
    }

    @Test
    func raisingTheVCPUCountTakesGuestMemoryDownToTheNewMaximum() {
        var configuration = EmulatorConfiguration(
            vCPUCount: 1, guestMemoryMiB: 2304, overlaySizeGiB: 8,
            androidDiskSizeGiB: 8, rootfsFilename: "x.erofs",
            retinaMode: false, autoResizeGuest: true, androidBootMode: .automatic
        )
        let bound = Self.mainGuestMemoryBound
        #expect(
            configuration.maximumGuestMemoryMiB(vCPUCount: 1, bound: bound) ==
                2304
        )
        #expect(
            configuration.maximumGuestMemoryMiB(vCPUCount: 6, bound: bound) ==
                2176
        )

        // Raising the count clamps, in the stepper's steps.
        configuration.vCPUCount = 3
        configuration.vCPUCountChanged(from: 1, bound: bound)
        #expect(configuration.guestMemoryMiB == 2240)
        configuration.vCPUCount = 6
        configuration.vCPUCountChanged(from: 3, bound: bound)
        #expect(configuration.guestMemoryMiB == 2176)
        #expect(configuration.guestMemoryMiB % 64 == 0)

        // Lowering it leaves the size alone, even above the new maximum of a
        // size the environment stated.
        configuration.vCPUCount = 1
        configuration.vCPUCountChanged(from: 6, bound: bound)
        #expect(configuration.guestMemoryMiB == 2176)
        configuration.guestMemoryMiB = 4096
        configuration.vCPUCount = 1
        configuration.vCPUCountChanged(from: 2, bound: bound)
        #expect(configuration.guestMemoryMiB == 4096)

        // A size under the new maximum stays as it is.
        configuration.guestMemoryMiB = 1024
        configuration.vCPUCount = 8
        configuration.vCPUCountChanged(from: 1, bound: bound)
        #expect(configuration.guestMemoryMiB == 1024)

        // The maximum is a size of the stepper's range, whatever the bound
        // says.
        for (said, expected) in [(0, 512), (600, 576), (2150, 2112), (99999, 4096)] {
            #expect(
                configuration.maximumGuestMemoryMiB(
                    vCPUCount: 4, bound: { _ in said }
                ) == expected
            )
        }
    }

    @Test
    func gpuBudgetWarningNamesTheShortfallOnlyWhenTheFloorIsMissed() {
        var plan = kernarium_runtime_gpu_budget()
        plan.struct_size = UInt32(
            MemoryLayout<kernarium_runtime_gpu_budget>.size
        )
        plan.budget_bytes = 384 << 20
        plan.process_limit_bytes = 2867 << 20
        plan.guest_memory_bytes = 2048 << 20
        plan.available_bytes = 273 << 20
        plan.short_of_floor = 0
        #expect(GPUBudgetWarning(plan: plan) == nil)

        plan.short_of_floor = 1
        let warning = GPUBudgetWarning(plan: plan)
        #expect(warning?.limitMiB == 2867)
        #expect(warning?.guestMemoryMiB == 2048)
        #expect(warning?.availableMiB == 273)
        #expect(warning?.floorMiB == 384)
        #expect(warning?.message.contains("2867 MiB") == true)
        #expect(warning?.message.contains("273 MiB") == true)
        #expect(warning?.message.contains("likely") == true)
    }

    @Test
    func savedConfigurationUsesTheStoredKeysEveryBuildWrote() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }

        EmulatorConfiguration(
            vCPUCount: 3,
            guestMemoryMiB: 1536,
            overlaySizeGiB: 12,
            androidDiskSizeGiB: 20,
            rootfsFilename: "phosh.erofs",
            retinaMode: true,
            autoResizeGuest: false,
            androidBootMode: .normal
        ).save(defaults: defaults)

        #expect(defaults.integer(forKey: "emulator.vCPUCount") == 3)
        #expect(defaults.integer(forKey: "emulator.guestMemoryMiB") == 1536)
        #expect(defaults.integer(forKey: "emulator.overlaySizeGiB") == 12)
        #expect(defaults.integer(forKey: "emulator.androidDiskSizeGiB") == 20)
        #expect(defaults.string(forKey: "emulator.rootfsFilename") == "phosh.erofs")
        #expect(defaults.object(forKey: "emulator.retinaMode") as? Bool == true)
        #expect(
            defaults.object(forKey: "emulator.autoResizeGuest") as? Bool == false
        )
        #expect(defaults.integer(forKey: "emulator.androidBootMode") == 1)
        // Nothing else is stored.
        #expect(
            defaults.dictionaryRepresentation().keys
                .filter { $0.hasPrefix("emulator.") }.count == 8
        )
    }

    @Test
    func storedValuesOfTheWrongKindFallBackToTheirBounds() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }
        defaults.set("four", forKey: "emulator.vCPUCount")
        defaults.set("big", forKey: "emulator.guestMemoryMiB")
        defaults.set("x", forKey: "emulator.overlaySizeGiB")
        defaults.set("x", forKey: "emulator.androidDiskSizeGiB")
        defaults.set(99, forKey: "emulator.androidBootMode")

        let configuration = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6
        )

        #expect(configuration.vCPUCount == 1)
        #expect(configuration.guestMemoryMiB == 512)
        #expect(configuration.overlaySizeGiB == 1)
        #expect(configuration.androidDiskSizeGiB == 6)
        #expect(configuration.androidBootMode == .automatic)

        // Only "0" and "1" choose a switch from the environment.
        for ignored in ["", " 1", "yes", "true", "01"] {
            let loaded = EmulatorConfiguration.load(
                defaults: defaults,
                physicalProcessorCount: 6,
                environment: [
                    EmulatorConfiguration.retinaEnvironmentKey: ignored,
                    EmulatorConfiguration.autoResizeGuestEnvironmentKey: ignored,
                ]
            )
            #expect(loaded.retinaMode == EmulatorConfiguration.defaultRetinaMode)
            #expect(
                loaded.autoResizeGuest ==
                    EmulatorConfiguration.defaultAutoResizeGuest
            )
        }
    }

    @Test
    func storedUnifiedImageSelectionOutlivesTheDesktopDefault() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }
        defaults.set("rootfs.erofs", forKey: "emulator.rootfsFilename")

        let configuration = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6
        )

        #expect(configuration.rootfsFilename == "rootfs.erofs")
        configuration.save(defaults: defaults)
        #expect(
            defaults.string(forKey: "emulator.rootfsFilename") ==
                "rootfs.erofs"
        )
    }

    @Test
    func environmentCanFreezePerformanceConfiguration() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }
        defaults.set(2, forKey: "emulator.vCPUCount")
        defaults.set(2048, forKey: "emulator.guestMemoryMiB")
        defaults.set(16, forKey: "emulator.overlaySizeGiB")
        defaults.set(true, forKey: "emulator.retinaMode")
        defaults.set(false, forKey: "emulator.autoResizeGuest")
        defaults.set(
            AndroidBootMode.normal.rawValue,
            forKey: "emulator.androidBootMode"
        )

        let selected = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6,
            environment: [
                EmulatorConfiguration.vCPUEnvironmentKey: "4",
                EmulatorConfiguration.guestMemoryEnvironmentKey: "1536",
                EmulatorConfiguration.overlaySizeEnvironmentKey: "8",
                EmulatorConfiguration.retinaEnvironmentKey: "0",
                EmulatorConfiguration.autoResizeGuestEnvironmentKey: "1",
                AndroidBootMode.environmentKey: "recovery"
            ],
            guestMemoryBound: Self.unboundedGuestMemory
        )
        #expect(selected.vCPUCount == 4)
        #expect(selected.guestMemoryMiB == 1536)
        #expect(selected.overlaySizeGiB == 8)
        #expect(selected.retinaMode == false)
        #expect(selected.autoResizeGuest == true)
        #expect(selected.androidBootMode == .recovery)

        let invalid = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6,
            environment: [
                EmulatorConfiguration.vCPUEnvironmentKey: "7",
                EmulatorConfiguration.guestMemoryEnvironmentKey: "1500",
                EmulatorConfiguration.overlaySizeEnvironmentKey: "0",
                EmulatorConfiguration.retinaEnvironmentKey: "false",
                EmulatorConfiguration.autoResizeGuestEnvironmentKey: "true",
                AndroidBootMode.environmentKey: "invalid"
            ],
            guestMemoryBound: Self.unboundedGuestMemory
        )
        #expect(invalid.vCPUCount == 2)
        #expect(invalid.guestMemoryMiB == 2048)
        #expect(invalid.overlaySizeGiB == 16)
        #expect(invalid.retinaMode == true)
        #expect(invalid.autoResizeGuest == false)
        #expect(invalid.androidBootMode == .normal)
    }

    @Test
    func rootfsSelectionAcceptsOnlyDocumentsFilenames() {
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "phosh.erofs",
                environment: [:]
            ) == "phosh.erofs"
        )
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "pmos-sxmo.erofs",
                environment: [:]
            ) == "pmos-sxmo.erofs"
        )
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "rootfs-gl-gles.ext4",
                environment: [:]
            ) == "rootfs-gl-gles.ext4"
        )
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "rootfs.erofs",
                environment: [RootfsImage.environmentKey: "phosh.erofs"]
            ) == "phosh.erofs"
        )
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "rootfs.erofs",
                environment: [RootfsImage.environmentKey: "../phosh.erofs"]
            ) == nil
        )
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "rootfs.erofs",
                environment: [RootfsImage.environmentKey: "phosh.img"]
            ) == nil
        )
        // The name ends up in a UBD kernel argument, which these would split
        // or misread.
        for unusable in [
            "My Disk.ext4", "tab\tdisk.erofs", "a,b.erofs", "a:b.erofs",
            "quote\".erofs", "\u{00A0}nbsp.erofs", "\u{8800}.erofs"
        ] {
            #expect(RootfsImage.validatedFilename(unusable) == nil)
        }
        #expect(RootfsImage.validatedFilename("my-disk_2.ext4") == "my-disk_2.ext4")
    }

    @Test
    func rootfsEnumerationIncludesOnlyRegularSupportedImages() throws {
        let manager = FileManager.default
        let directory = try makeScratchDirectory("rootfs-enumeration")
        defer { try? manager.removeItem(at: directory) }

        #expect(
            manager.createFile(
                atPath: directory.appendingPathComponent("phosh.erofs").path,
                contents: Data()
            )
        )
        #expect(
            manager.createFile(
                atPath: directory.appendingPathComponent("rootfs.erofs").path,
                contents: Data()
            )
        )
        #expect(
            manager.createFile(
                atPath: directory
                    .appendingPathComponent("pmos-sxmo.erofs").path,
                contents: Data()
            )
        )
        #expect(
            manager.createFile(
                atPath: directory
                    .appendingPathComponent("rootfs-gl-gles.ext4").path,
                contents: Data()
            )
        )
        #expect(
            manager.createFile(
                atPath: directory.appendingPathComponent("ignored.img").path,
                contents: Data()
            )
        )
        try manager.createDirectory(
            at: directory.appendingPathComponent("directory.erofs"),
            withIntermediateDirectories: false
        )

        #expect(
            RootfsImage.availableFilenames(in: directory) == [
                "phosh.erofs",
                "pmos-sxmo.erofs",
                "rootfs-gl-gles.ext4",
                "rootfs.erofs",
            ]
        )
    }

    @Test
    func zstdUtilReportsProgressAndCreatesSparseOutput() throws {
        let manager = FileManager.default
        let directory = try makeScratchDirectory("zstd-sparse")
        defer { try? manager.removeItem(at: directory) }

        let source = directory.appendingPathComponent("disk.img.zst")
        let destination = directory.appendingPathComponent("disk.img")
        try Self.zstdFixture.write(to: source)
        var reports: [ZstdUtil.DecompressionProgress] = []

        try ZstdUtil.decompress(
            from: source,
            to: destination,
            sparse: true
        ) { reports.append($0) }

        let attributes = try manager.attributesOfItem(atPath: destination.path)
        #expect((attributes[.size] as? NSNumber)?.intValue == Self.zstdOutputSize)
        let handle = try FileHandle(forReadingFrom: destination)
        var status = stat()
        let statResult = Darwin.fstat(handle.fileDescriptor, &status)
        #expect(statResult == 0)
        #expect(
            (reports.last?.sparseBytesSkipped ?? 0) >
                UInt64(Self.zstdOutputSize - 256 * 1024)
        )
        #expect(Int(status.st_blocks) * 512 < Self.zstdOutputSize / 2)

        let prefix = try handle.read(upToCount: Self.zstdPrefix.count)
        try handle.seek(
            toOffset: UInt64(Self.zstdOutputSize - Self.zstdSuffix.count)
        )
        let suffix = try handle.read(upToCount: Self.zstdSuffix.count)
        try handle.close()
        #expect(prefix == Self.zstdPrefix)
        #expect(suffix == Self.zstdSuffix)
        #expect(reports.first?.compressedBytesConsumed == 0)
        #expect(
            reports.last?.compressedBytesConsumed ==
                UInt64(Self.zstdFixture.count)
        )
        #expect(
            reports.last?.compressedBytesTotal ==
                UInt64(Self.zstdFixture.count)
        )
        #expect(
            reports.last?.decompressedBytesWritten ==
                UInt64(Self.zstdOutputSize)
        )
        #expect(reports.last?.fractionCompleted == 1)
    }

    /// Two frames: 1000 bytes of 0xAB, then 200000 zeros and "END\n". The
    /// hole starts and ends inside filesystem blocks, which F_PUNCHHOLE
    /// refuses unless the range is rounded to whole blocks.
    @Test
    func zstdUtilPunchesHolesThatAreNotBlockAligned() throws {
        let manager = FileManager.default
        let directory = try makeScratchDirectory("zstd-unaligned")
        defer { try? manager.removeItem(at: directory) }

        let source = directory.appendingPathComponent("unaligned.zst")
        let destination = directory.appendingPathComponent("unaligned.img")
        try Data([
            0x28, 0xB5, 0x2F, 0xFD, 0x60, 0xE8, 0x02, 0x4D, 0x00, 0x00,
            0x10, 0xAB, 0xAB, 0x01, 0x00, 0xE3, 0x2B, 0x80, 0x05, 0x28,
            0xB5, 0x2F, 0xFD, 0xA0, 0x44, 0x0D, 0x03, 0x00, 0x54, 0x00,
            0x00, 0x10, 0x00, 0x00, 0x01, 0x00, 0xFB, 0xFF, 0x39, 0xC0,
            0x02, 0x6D, 0x00, 0x00, 0x28, 0x00, 0x45, 0x4E, 0x44, 0x0A,
            0x01, 0x00, 0x3C, 0x0D, 0x39, 0x10, 0x02,
        ]).write(to: source)

        try ZstdUtil.decompress(from: source, to: destination, sparse: true)

        let output = try Data(contentsOf: destination)
        #expect(output.count == 201_004)
        #expect(output.prefix(1000).allSatisfy { $0 == 0xAB })
        #expect(output[1000..<201_000].allSatisfy { $0 == 0 })
        #expect(output.suffix(4) == Data("END\n".utf8))
    }

    @Test
    func zstdUtilLeavesExistingDestinationUntouchedOnFailure() throws {
        let manager = FileManager.default
        let directory = try makeScratchDirectory("zstd-failure")
        defer { try? manager.removeItem(at: directory) }

        let source = directory.appendingPathComponent("invalid.zst")
        let destination = directory.appendingPathComponent("existing.img")
        let original = Data("existing destination".utf8)
        try Data([0x00, 0x01, 0x02, 0x03]).write(to: source)
        try original.write(to: destination)

        var failed = false
        do {
            try ZstdUtil.decompress(from: source, to: destination)
        } catch {
            failed = true
        }
        #expect(failed)
        #expect(try Data(contentsOf: destination) == original)
        #expect(
            try manager.contentsOfDirectory(atPath: directory.path).sorted() == [
                "existing.img",
                "invalid.zst",
            ]
        )
    }

    private func isolatedDefaults() -> (UserDefaults, String) {
        let suiteName = "KernariumTests.\(UUID().uuidString)"
        return (UserDefaults(suiteName: suiteName)!, suiteName)
    }
}
