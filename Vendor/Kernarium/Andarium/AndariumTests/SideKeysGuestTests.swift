// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
import UIKit
@testable import Andarium

/// Opt-in (`ANDARIUM_SIDE_KEYS_TESTS=1`, selecting only this suite): a real
/// Android behind the production scene root with the side keys open. UIKit
/// hit-tests each point of a touch in the real window, the test hands the
/// touches that reach the guest's display to that real view, and the guest's
/// touchscreen node shows what Android received.
///
/// A touch beside the open keys must reach the display; the first event of
/// its contact must arrive at Android, with the keys already closed once the
/// display has handled the beginning. A tap, a swipe and two fingers each do
/// that. A point on a key must stay with the keys.
@MainActor
struct SideKeysGuestTests {
    /// What UIKit would hand the display: a contact at a point of the view.
    private final class Finger: UITouch {
        var point = CGPoint.zero
        override func location(in view: UIView?) -> CGPoint { point }
    }

    private struct Record {
        let type: UInt16
        let code: UInt16
        let value: Int32

        static let touchButton: UInt16 = 0x14a
        static let slot: UInt16 = 0x2f
        static let trackingId: UInt16 = 0x39
        static let positionX: UInt16 = 0x35
    }

    private static let capture = "/data/local/tmp/andarium-side-keys.evdev"

    private static func decode(_ data: Data) -> [Record] {
        // 64-bit struct timeval, then type, code and value.
        let size = 24
        return stride(from: 0, to: data.count - size + 1, by: size).map { offset in
            let record = data.subdata(in: offset..<offset + size)
            return Record(
                type: record.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 16, as: UInt16.self) },
                code: record.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 18, as: UInt16.self) },
                value: record.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 20, as: Int32.self) })
        }
    }

    private func touchscreenNode(_ operations: AdbGuestOperations) async throws -> String {
        let text = try await operations.shell("getevent -p -l", limit: .seconds(15)).text
        let device = try #require(
            text.components(separatedBy: "add device ").dropFirst()
                .first { $0.contains("\"Kernarium Touchscreen\"") },
            "getevent lists no Kernarium Touchscreen: \(text)")
        let line = try #require(device.split(separator: "\n").first)
        let path = try #require(line.split(separator: ":").last).trimmingCharacters(in: .whitespaces)
        try #require(path.hasPrefix("/dev/input/event"), "\(line)")
        return path
    }

    /// Reads the node into a file in the guest, bounded to two minutes, for
    /// the length of `body`, and returns what it delivered.
    private func reading(
        _ operations: AdbGuestOperations, node: String, during body: () async throws -> Void
    ) async throws -> [Record] {
        let started = try await operations.shell(
            "rm -f \(Self.capture); nohup timeout 120 cat \(node) > \(Self.capture) 2>/dev/null </dev/null & echo $!",
            limit: .seconds(15)).text.trimmingCharacters(in: .whitespacesAndNewlines)
        let reader = try #require(Int(started), "no reader process: \(started)")
        try await Task.sleep(for: .seconds(1))
        try await body()
        try await Task.sleep(for: .seconds(1))
        let data = try await operations.shell("kill \(reader); cat \(Self.capture); rm -f \(Self.capture)",
            limit: .seconds(15)).stdout
        return Self.decode(data)
    }

    private func eventually(seconds: Int = 5, _ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(seconds)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(20))
        }
        return condition()
    }

    private func descendants(of view: UIView) -> [UIView] {
        view.subviews + view.subviews.flatMap { descendants(of: $0) }
    }

    @Test(.timeLimit(.minutes(25)))
    func touchesBesideTheOpenKeysReachAndroidAndCloseThem() async throws {
        guard ProcessInfo.processInfo.environment["ANDARIUM_SIDE_KEYS_TESTS"] == "1" else { return }
        try await withAndroidGuest { model, operations in
            _ = try await operations.shell("input keyevent KEYCODE_WAKEUP")
            let node = try await touchscreenNode(operations)
            #expect(await eventually(seconds: 60) { model.phase == .running(.normal) })

            let scene = try #require(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
            let window = try #require(scene.windows.first { $0.isKeyWindow })
            let root = try #require(window.rootViewController?.view)
            let guest = try #require(descendants(of: root).compactMap { $0 as? GuestMetalView }.first)
            #expect(guest.bounds.width > 0)

            // Where the overlay lays out: the safe area, from its top-left.
            let safe = root.safeAreaInsets
            let width = root.bounds.width - safe.left - safe.right
            let height = root.bounds.height - safe.top - safe.bottom
            func inWindow(_ x: CGFloat, _ y: CGFloat) -> CGPoint {
                root.convert(CGPoint(x: safe.left + x, y: safe.top + y), to: window)
            }
            func reachesDisplay(_ point: CGPoint) -> Bool {
                guard let view = window.hitTest(point, with: nil) else { return false }
                return view === guest || view.isDescendant(of: guest)
            }
            func finger(at point: CGPoint) -> Finger {
                let touch = Finger()
                touch.point = guest.convert(point, from: window)
                return touch
            }
            /// The fraction of the display's width a contact reported at `point`.
            let range = Double(KERNARIUM_RUNTIME_TOUCH_MAXIMUM) + 1
            func column(of point: CGPoint) -> Double { Double(guest.convert(point, from: window).x / guest.bounds.width) }
            func openKeys() async throws {
                model.session.sideKeysOpen = true
                try await Task.sleep(for: .milliseconds(800))
                root.layoutIfNeeded()
            }

            let tap = inWindow(width * 0.35, height * 0.50)
            let swipeFrom = inWindow(width * 0.15, height * 0.60)
            let swipeTo = inWindow(width * 0.55, height * 0.60)
            let second = inWindow(width * 0.60, height * 0.40)

            // The keys sit at the edge: a point on one stays with the keys.
            let groupWidth = KeyMetrics.width * 2 + KeyMetrics.columnGap
            let centreY = min(max(118, height * 0.32), max(118, height - 118))
            let groupTop = centreY - (KeyMetrics.keyHeight + KeyMetrics.rowGap + 2 * (KeyMetrics.keyHeight - 0.75) + 1.5) / 2
            let keyPoints = [
                inWindow(width - KeyMetrics.edgeGap - KeyMetrics.width / 2, groupTop + KeyMetrics.keyHeight / 2),
                inWindow(width - KeyMetrics.edgeGap - groupWidth + KeyMetrics.width / 2, groupTop + KeyMetrics.keyHeight / 2),
            ]

            // A tap: down and up at the point, keys closed when the display
            // has handled the beginning.
            try await openKeys()
            #expect(model.session.sideKeysOpen)
            #expect(reachesDisplay(tap), "the open keys take a touch beside them")
            for point in keyPoints { #expect(!reachesDisplay(point), "a key at \(point) lets the touch through") }
            let tapped = try await reading(operations, node: node) {
                let touch = finger(at: tap)
                guest.touchesBegan([touch], with: nil)
                #expect(!model.session.sideKeysOpen, "the keys closed with the beginning of the touch")
                try await Task.sleep(for: .milliseconds(120))
                guest.touchesEnded([touch], with: nil)
            }
            #expect(tapped.contains { $0.type == 1 && $0.code == Record.touchButton && $0.value == 1 },
                "no BTN_TOUCH down: \(tapped.count) records")
            #expect(tapped.contains { $0.type == 1 && $0.code == Record.touchButton && $0.value == 0 }, "no lift")
            // The contact's first position is where the touch began.
            let tapX = try #require(tapped.first { $0.type == 3 && $0.code == Record.positionX })
            #expect(abs(Double(tapX.value) / range - column(of: tap)) < 0.03, "first x \(tapX.value)")
            #expect(tapped.contains { $0.type == 3 && $0.code == Record.trackingId && $0.value >= 0 })
            print("SIDE_KEYS tap: \(tapped.count) records, first x \(tapX.value) of \(Int(range)), expected \(Int(column(of: tap) * range))")
            #expect(await eventually { !model.session.sideKeysOpen })

            // A swipe: the contact moves across the display and lifts.
            try await openKeys()
            #expect(reachesDisplay(swipeFrom))
            let swiped = try await reading(operations, node: node) {
                let touch = finger(at: swipeFrom)
                guest.touchesBegan([touch], with: nil)
                #expect(!model.session.sideKeysOpen)
                for step in 1...12 {
                    let t = CGFloat(step) / 12
                    touch.point = guest.convert(
                        CGPoint(x: swipeFrom.x + (swipeTo.x - swipeFrom.x) * t, y: swipeFrom.y), from: window)
                    guest.touchesMoved([touch], with: nil)
                    try await Task.sleep(for: .milliseconds(30))
                }
                guest.touchesEnded([touch], with: nil)
            }
            let xs = swiped.filter { $0.type == 3 && $0.code == Record.positionX }.map(\.value)
            #expect(Set(xs).count >= 6, "swipe positions \(xs)")
            #expect(abs(Double(xs.first ?? -1) / range - column(of: swipeFrom)) < 0.03, "swipe began at \(xs.first ?? -1)")
            #expect(abs(Double(xs.last ?? -1) / range - column(of: swipeTo)) < 0.03, "swipe ended at \(xs.last ?? -1)")
            #expect(xs == xs.sorted(), "the swipe moved back")
            print("SIDE_KEYS swipe: \(swiped.count) records, x \(xs.first ?? -1) through \(xs.last ?? -1) in \(Set(xs).count) positions")
            #expect(swiped.contains { $0.type == 1 && $0.code == Record.touchButton && $0.value == 1 })
            #expect(swiped.contains { $0.type == 1 && $0.code == Record.touchButton && $0.value == 0 })

            // Two fingers: the first beginning closes the keys, and both
            // contacts reach Android.
            try await openKeys()
            let pinched = try await reading(operations, node: node) {
                let first = finger(at: swipeFrom), other = finger(at: second)
                guest.touchesBegan([first], with: nil)
                #expect(!model.session.sideKeysOpen)
                guest.touchesBegan([other], with: nil)
                try await Task.sleep(for: .milliseconds(120))
                other.point = guest.convert(inWindow(width * 0.70, height * 0.30), from: window)
                guest.touchesMoved([other], with: nil)
                try await Task.sleep(for: .milliseconds(120))
                guest.touchesEnded([first, other], with: nil)
            }
            let slots = Set(pinched.filter { $0.type == 3 && $0.code == Record.slot }.map(\.value))
            let tracked = Set(pinched.filter { $0.type == 3 && $0.code == Record.trackingId && $0.value >= 0 }.map(\.value))
            #expect(slots.count == 2 && tracked.count == 2, "slots \(slots) tracking ids \(tracked)")
            print("SIDE_KEYS two fingers: \(pinched.count) records, slots \(slots.sorted()), tracking ids \(tracked.count)")

            // Closed, the keys leave everything to the guest; a point on a key
            // is the keys' again once they open, and no touch reached Android
            // while they did.
            try await openKeys()
            let idle = try await reading(operations, node: node) {
                for point in keyPoints { #expect(!reachesDisplay(point)) }
                try await Task.sleep(for: .seconds(1))
            }
            #expect(idle.isEmpty, "a touchscreen event with no touch: \(idle.count)")
            print("SIDE_KEYS keys: touchscreen records while open and idle \(idle.count), keys open \(model.session.sideKeysOpen)")
            #expect(model.session.sideKeysOpen)
        }
    }
}
