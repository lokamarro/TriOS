// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// The app's mark drawn live, as the Dusk Jade icon draws it: three turned
/// plates, the app plate at the bottom, the kernel, and the guest on top. A
/// lit guest is celadon with a soft light behind the stack; an unlit guest is
/// dark slate, and a guest that stopped unexpectedly shows a red light.
struct StackMark: View {
    var lit: Bool
    var width: CGFloat = 150
    /// Red light for a guest that stopped unexpectedly.
    var fault = false

    var body: some View {
        let stack = StackGeometry.shared
        // The light reaches well past the plates, and a canvas clips to its
        // frame, so the canvas has a margin that takes no room in the layout.
        let margin = width * 0.5
        Canvas { context, size in
            let scale = (size.width - margin * 2) / stack.bounds.width
            let place = CGAffineTransform(translationX: margin, y: margin)
                .scaledBy(x: scale, y: scale)
                .translatedBy(x: -stack.bounds.minX, y: -stack.bounds.minY)
            let guestFace = Self.path(stack.plates[2].face, place)
            if lit || fault {
                Self.drawLight(around: guestFace.boundingRect, color: fault ? .red : Self.light, in: &context)
            }
            for (plate, colours) in zip(stack.plates, [Self.app, Self.kernel, lit ? Self.litGuest : Self.unlitGuest]) {
                let face = Self.path(plate.face, place)
                let silhouette = Self.path(plate.silhouette, place)
                context.drawLayer { layer in
                    layer.addFilter(.shadow(color: .black.opacity(0.32), radius: 9 * scale, y: 9 * scale))
                    let side = silhouette.boundingRect
                    layer.fill(silhouette, with: .linearGradient(
                        Gradient(colors: [colours.side.0, colours.side.1]),
                        startPoint: CGPoint(x: side.minX, y: side.midY),
                        endPoint: CGPoint(x: side.maxX, y: side.midY)
                    ))
                    let top = face.boundingRect
                    layer.fill(face, with: .linearGradient(
                        Gradient(colors: [colours.face.0, colours.face.1]),
                        startPoint: CGPoint(x: top.midX, y: top.minY),
                        endPoint: CGPoint(x: top.midX, y: top.maxY)
                    ))
                }
                context.stroke(face, with: .color(.white.opacity(0.4)), lineWidth: max(0.5, 2.5 * scale))
            }
        }
        .frame(width: width + margin * 2, height: width * stack.bounds.height / stack.bounds.width + margin * 2)
        .padding(-margin)
        .accessibilityHidden(true)
    }

    private typealias Plate = (face: (Color, Color), side: (Color, Color))

    // The icon's sRGB colours: faces run top to bottom, side bands left to right.
    private static let app: Plate = ((Color(hex: 0x1F8292), Color(hex: 0x197080)), (Color(hex: 0x208696), Color(hex: 0x166876)))
    private static let kernel: Plate = ((Color(hex: 0x2EA6A4), Color(hex: 0x24948F)), (Color(hex: 0x2CA09E), Color(hex: 0x1A7C78)))
    private static let litGuest: Plate = ((Color(hex: 0xF0FFFA), Color(hex: 0xBCEAE2)), (Color(hex: 0xA8E0DE), Color(hex: 0x338E90)))
    private static let unlitGuest: Plate = ((Color(hex: 0x50706D), Color(hex: 0x3A5654)), (Color(hex: 0x46625F), Color(hex: 0x1F3433)))
    private static let light = Color(hex: 0xCDF3E8)

    private static func path(_ points: [CGPoint], _ transform: CGAffineTransform) -> Path {
        Path { path in
            path.addLines(points)
            path.closeSubpath()
        }
        .applying(transform)
    }

    /// The icon's light: an ellipse centred just above the guest's face, fading
    /// from 60% at its centre.
    private static func drawLight(around face: CGRect, color: Color, in context: inout GraphicsContext) {
        let radiusX = face.width * 0.95
        let radiusY = face.height * 1.15
        let centre = CGPoint(x: face.midX, y: face.midY - face.height * 0.3)
        var glow = context
        glow.translateBy(x: centre.x, y: centre.y)
        glow.scaleBy(x: 1, y: radiusY / radiusX)
        let circle = Path(ellipseIn: CGRect(x: -radiusX, y: -radiusX, width: radiusX * 2, height: radiusX * 2))
        glow.fill(circle, with: .radialGradient(
            Gradient(stops: [
                .init(color: color.opacity(0.6), location: 0),
                .init(color: color.opacity(0.21), location: 0.45),
                .init(color: color.opacity(0), location: 1),
            ]),
            center: .zero, startRadius: 0, endRadius: radiusX
        ))
    }
}

/// The icon's plates in its own pixels: rounded squares turned −8°, +10° and
/// +28°, at heights 0, 112 and 266 px, each 40 px thick, seen from a 45°
/// azimuth with a vertical squash of 0.576.
private struct StackGeometry: Sendable {
    struct Plate: Sendable {
        var face: [CGPoint]
        /// The outline of the whole plate: its face and the side band below it.
        var silhouette: [CGPoint]
    }

    static let shared = StackGeometry()

    let plates: [Plate]
    let bounds: CGRect

    private init() {
        let thickness = 40.0
        plates = [(-8.0, 0.0), (10.0, 112.0), (28.0, 266.0)].map { turn, height in
            let outline = Self.roundedSquare(turnedBy: turn)
            let face = outline.map { Self.project($0, height: height) }
            let bottom = outline.map { Self.project($0, height: height - thickness) }
            return Plate(face: face, silhouette: Self.hull(face + bottom))
        }
        let points = plates.flatMap(\.silhouette)
        let xs = points.map(\.x), ys = points.map(\.y)
        bounds = CGRect(x: xs.min()!, y: ys.min()!, width: xs.max()! - xs.min()!, height: ys.max()! - ys.min()!)
    }

    /// A square from −1 to 1 with corners of radius 0.24.
    private static func roundedSquare(turnedBy degrees: Double) -> [CGPoint] {
        let radius = 0.24, inset = 1 - radius
        var points: [CGPoint] = []
        for (x, y, start) in [(inset, inset, 0.0), (-inset, inset, 90.0), (-inset, -inset, 180.0), (inset, -inset, 270.0)] {
            for step in 0...12 {
                let angle = (start + 7.5 * Double(step)) * .pi / 180
                points.append(CGPoint(x: x + radius * cos(angle), y: y + radius * sin(angle)))
            }
        }
        let turn = degrees * .pi / 180
        return points.map { CGPoint(x: $0.x * cos(turn) - $0.y * sin(turn), y: $0.x * sin(turn) + $0.y * cos(turn)) }
    }

    private static func project(_ point: CGPoint, height: Double) -> CGPoint {
        let scale = 160 * 2.squareRoot(), axis = 0.5.squareRoot()
        let across = (point.x - point.y) * axis
        let depth = (point.x + point.y) * axis
        return CGPoint(x: across * scale, y: depth * scale * 0.576 - height)
    }

    /// Convex hull, by Andrew's monotone chain.
    private static func hull(_ points: [CGPoint]) -> [CGPoint] {
        let sorted = points.sorted { $0.x != $1.x ? $0.x < $1.x : $0.y < $1.y }
        func cross(_ o: CGPoint, _ a: CGPoint, _ b: CGPoint) -> Double {
            (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x)
        }
        var lower: [CGPoint] = [], upper: [CGPoint] = []
        for point in sorted {
            while lower.count >= 2, cross(lower[lower.count - 2], lower[lower.count - 1], point) <= 0 { lower.removeLast() }
            lower.append(point)
        }
        for point in sorted.reversed() {
            while upper.count >= 2, cross(upper[upper.count - 2], upper[upper.count - 1], point) <= 0 { upper.removeLast() }
            upper.append(point)
        }
        return Array(lower.dropLast() + upper.dropLast())
    }
}

extension Color {
    init(hex: UInt32) {
        self.init(
            red: Double((hex >> 16) & 0xFF) / 255,
            green: Double((hex >> 8) & 0xFF) / 255,
            blue: Double(hex & 0xFF) / 255
        )
    }
}
