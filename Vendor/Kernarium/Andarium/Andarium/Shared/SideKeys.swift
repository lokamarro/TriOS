// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import TipKit

/// What the side keys do. The Simulator preview plays them against mock
/// screens; the device application sends them to Android.
struct SideKeyActions {
    var power: () -> Void
    var powerHold: () -> Void
    var volumeUp: () -> Void
    var volumeDown: () -> Void
    var back: () -> Void
    var home: () -> Void
    var settings: () -> Void
}

/// The side keys on the screen edge: closed they are a pair of slim glass
/// keys; tapped they swell in place into Power and a volume rocker, with
/// Settings and a Back and Home pair right beside them as one small group.
/// They move up and down the edge with a drag and across to the other edge.
///
/// Only the keys take touches, and the tip beside the closed keys on its own
/// card. While they are open, a touch anywhere else reaches the guest as it
/// would with them closed, and the guest's display closes the keys when it
/// receives that touch (`SessionState.closeSideKeys`).
struct SideKeysOverlay: View {
    /// How the keys open and close.
    static let motion = Animation.spring(duration: 0.36, bounce: 0.22)

    let mode: StartMode
    /// Whether the guest's screen is on, for Power's light.
    let lit: Bool
    /// Whether the keys are open. The session owns it, so the guest's
    /// display can close them.
    @Binding var open: Bool
    let actions: SideKeyActions

    @State private var leading: Bool
    @State private var fraction: CGFloat = 0.32
    @State private var dragStart: CGPoint?
    /// Counts key presses for the click felt on each one.
    @State private var keyPresses = 0
    @Namespace private var keysSpace
    private let tip = SideKeysTip()
    /// The tip's popover is up: while the closed keys are on screen and the
    /// tip wants to show.
    @State private var tipShown = false

    init(mode: StartMode, lit: Bool, open: Binding<Bool>, leading: Bool = false, actions: SideKeyActions) {
        self.mode = mode
        self.lit = lit
        _open = open
        self.actions = actions
        _leading = State(initialValue: leading)
    }

    var body: some View {
        GeometryReader { geo in
            keys(in: geo.size)
        }
        .ignoresSafeArea(.keyboard)
        .sensoryFeedback(.impact(weight: .light), trigger: keyPresses)
    }

    @ViewBuilder
    private func keys(in size: CGSize) -> some View {
        let edgeX: CGFloat = leading ? 0 : size.width
        let closedY = min(max(70, size.height * fraction), size.height - 70)
        let openY = min(max(118, size.height * fraction), max(118, size.height - 118))
        let inward: CGFloat = leading ? 1 : -1
        let recoveryMode = mode == .recovery
        // The group's width: Power's column, and in Android the column beside it.
        let groupWidth: CGFloat = recoveryMode ? KeyMetrics.width : KeyMetrics.width * 2 + KeyMetrics.columnGap

        ZStack {
            if open {
                // A soft shade behind the group keeps it legible over any
                // screen. It takes no touch: one beside the group reaches the
                // guest, which closes the keys.
                RadialGradient(
                    colors: [.black.opacity(0.5), .black.opacity(0.16), .black.opacity(0)],
                    center: UnitPoint(x: edgeX / max(size.width, 1), y: openY / max(size.height, 1)),
                    startRadius: 20, endRadius: 300
                )
                .allowsHitTesting(false)
                .transition(.opacity)

                GlassEffectContainer(spacing: 4) {
                    HStack(alignment: .top, spacing: KeyMetrics.columnGap) {
                        if leading {
                            powerColumn
                            if !recoveryMode { guestColumn }
                        } else {
                            if !recoveryMode { guestColumn }
                            powerColumn
                        }
                    }
                }
                .accessibilityAction(.escape) { setOpen(false) }
                .position(x: edgeX + inward * (KeyMetrics.edgeGap + groupWidth / 2), y: openY)
            } else {
                ClosedKeys(space: keysSpace, leading: leading)
                    // SwiftUI's own popover, not `popoverTip`: TipKit leaves
                    // its popover on screen when the keys it points at go
                    // away (opening, or the session ending), over the open
                    // keys or Home, where its card takes their touches.
                    // SwiftUI dismisses its popover with the closed keys.
                    .popover(isPresented: $tipShown, arrowEdge: leading ? .leading : .trailing) {
                        SideKeysTipCard(tip: tip)
                    }
                    .task {
                        for await shows in tip.shouldDisplayUpdates { tipShown = shows }
                    }
                    .onDisappear { tipShown = false }
                    .position(x: edgeX + inward * 18, y: closedY)
                    .gesture(dragKeys(size: size))
                    .onTapGesture { setOpen(true) }
            }
        }
    }

    private var powerColumn: some View {
        OpenKeys(
            space: keysSpace, recovery: mode == .recovery,
            lit: lit,
            power: { pressed(actions.power) }, powerHold: holdPower,
            volumeUp: { pressed(actions.volumeUp) }, volumeDown: { pressed(actions.volumeDown) }
        )
    }

    private var guestColumn: some View {
        GuestKeys(
            settings: openSettings,
            back: { pressed(actions.back) },
            home: { pressed(actions.home) }
        )
        .transition(
            .asymmetric(
                insertion: .scale(scale: 0.6, anchor: leading ? .leading : .trailing)
                    .combined(with: .opacity)
                    .animation(.spring(duration: 0.34, bounce: 0.25).delay(0.04)),
                removal: .opacity.animation(.easeOut(duration: 0.12))
            )
        )
    }

    private func pressed(_ action: () -> Void) {
        keyPresses += 1
        action()
    }

    /// Android's power menu opens over the guest, so the keys step aside.
    private func holdPower() {
        guard mode == .normal else { return }
        actions.powerHold()
        withAnimation(.snappy) { open = false }
    }

    private func openSettings() {
        setOpen(false)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { actions.settings() }
    }

    private func dragKeys(size: CGSize) -> some Gesture {
        DragGesture(minimumDistance: 6, coordinateSpace: .local)
            .onChanged { value in
                if dragStart == nil {
                    dragStart = CGPoint(x: leading ? 0 : size.width, y: size.height * fraction)
                }
                guard let start = dragStart else { return }
                let y = start.y + value.translation.height
                fraction = min(max(0.1, y / size.height), 0.9)
            }
            .onEnded { value in
                // Dragged across the middle: the keys move to the other edge.
                let x = value.location.x
                if leading && x > size.width * 0.6 {
                    withAnimation(.snappy) { leading = false }
                } else if !leading && x < size.width * 0.4 {
                    withAnimation(.snappy) { leading = true }
                }
                dragStart = nil
            }
    }

    private func setOpen(_ value: Bool) {
        withAnimation(Self.motion) { open = value }
        if value { tip.invalidate(reason: .actionPerformed) }
    }
}

// MARK: - Closed and open keys

enum KeyMetrics {
    static let width: CGFloat = 52
    static let keyHeight: CGFloat = 62
    static let rowGap: CGFloat = 10
    static let columnGap: CGFloat = 8
    static let edgeGap: CGFloat = 8
}

/// The closed keys: Power and a longer volume rocker, each a plain glass key
/// on the screen edge. They show nothing about Android's state.
struct ClosedKeys: View {
    let space: Namespace.ID
    let leading: Bool

    var body: some View {
        VStack(spacing: 6) {
            key(height: 30)
                .matchedGeometryEffect(id: "power", in: space)
            key(height: 58)
                .matchedGeometryEffect(id: "rocker", in: space)
        }
        .padding(leading ? .leading : .trailing, 3)
        .frame(width: 36, height: 124, alignment: leading ? .leading : .trailing)
        .contentShape(Rectangle())
        .accessibilityElement()
        .accessibilityLabel("Side keys")
        .accessibilityHint("Opens Power, volume, Back, Home and Settings")
        .accessibilityAddTraits(.isButton)
    }

    private func key(height: CGFloat) -> some View {
        Capsule()
            .fill(.white.opacity(0.9))
            .frame(width: 9, height: height)
            .glassEffect(.regular, in: .capsule)
            .shadow(color: .black.opacity(0.35), radius: 3, x: leading ? 1 : -1)
    }
}

/// The open keys, swollen in place: Power (tap to press, hold for Android's
/// power menu) above the volume rocker.
struct OpenKeys: View {
    let space: Namespace.ID
    let recovery: Bool
    let lit: Bool
    let power: () -> Void
    let powerHold: () -> Void
    let volumeUp: () -> Void
    let volumeDown: () -> Void

    var body: some View {
        VStack(spacing: KeyMetrics.rowGap) {
            ZStack {
                Capsule().fill(.clear)
                VStack(spacing: 3) {
                    Image(systemName: "power")
                        .font(.system(size: 20, weight: .semibold))
                    Circle()
                        .fill(lit ? Color.accentColor : Color.gray.opacity(0.5))
                        .frame(width: 5, height: 5)
                        .shadow(color: lit ? Color.accentColor : .clear, radius: 3)
                }
            }
            .frame(width: KeyMetrics.width, height: KeyMetrics.keyHeight)
            .glassEffect(.regular.interactive(), in: .capsule)
            .matchedGeometryEffect(id: "power", in: space)
            .contentShape(.capsule)
            .onTapGesture(perform: power)
            .onLongPressGesture(minimumDuration: 0.6, perform: powerHold)
            .accessibilityElement()
            .accessibilityLabel("Power")
            .accessibilityHint(recovery ? "Chooses the highlighted item" : "Hold for the power menu")
            .accessibilityAddTraits(.isButton)
            .accessibilityAction(named: "Power menu", powerHold)

            KeyPair(
                top: (recovery ? "chevron.up" : "plus", recovery ? "Up" : "Volume up", volumeUp),
                bottom: (recovery ? "chevron.down" : "minus", recovery ? "Down" : "Volume down", volumeDown)
            )
            .matchedGeometryEffect(id: "rocker", in: space)
        }
        .foregroundStyle(.primary)
    }
}

/// The keys Android has and an iPhone does not, beside Power and volume:
/// Settings across from Power, and Back above Home across from the rocker.
struct GuestKeys: View {
    let settings: () -> Void
    let back: () -> Void
    let home: () -> Void

    var body: some View {
        VStack(spacing: KeyMetrics.rowGap) {
            Button(action: settings) {
                Image(systemName: "gearshape")
                    .font(.system(size: 20, weight: .semibold))
                    .frame(width: KeyMetrics.width, height: KeyMetrics.keyHeight)
                    .contentShape(.capsule)
            }
            .buttonStyle(.plain)
            .glassEffect(.regular.interactive(), in: .capsule)
            .accessibilityLabel("Settings")
            .accessibilityHint("Install apps, ADB, stats and options")

            KeyPair(
                top: ("arrowtriangle.backward.fill", "Back", back),
                bottom: ("circle.fill", "Home", home),
                glyphSize: 16
            )
        }
        .foregroundStyle(.primary)
    }
}

/// Two keys in one glass capsule, like a volume rocker.
struct KeyPair: View {
    let top: (symbol: String, label: String, action: () -> Void)
    let bottom: (symbol: String, label: String, action: () -> Void)
    var glyphSize: CGFloat = 19

    var body: some View {
        VStack(spacing: 0) {
            key(top)
            Capsule()
                .fill(.primary.opacity(0.18))
                .frame(width: 22, height: 1.5)
            key(bottom)
        }
        .buttonStyle(.plain)
        .glassEffect(.regular.interactive(), in: .capsule)
    }

    private func key(_ key: (symbol: String, label: String, action: () -> Void)) -> some View {
        Button(action: key.action) {
            Image(systemName: key.symbol)
                .font(.system(size: glyphSize, weight: .semibold))
                .frame(width: KeyMetrics.width, height: KeyMetrics.keyHeight - 0.75)
                .contentShape(Rectangle())
        }
        .accessibilityLabel(key.label)
    }
}

// MARK: - Tip

/// Shown once Android has booted, pointing at the side keys. It stops once the
/// keys have been opened or the tip closed, and appears at most three times.
nonisolated struct SideKeysTip: Tip {
    var title: Text { Text("Side keys") }
    var message: Text? { Text("Tap for Back, Home and Settings. Hold Power for the power menu. Drag to move.") }
    var image: Image? { Image(systemName: "hand.tap") }
    var options: [any TipOption] { [MaxDisplayCount(3)] }
}

/// The tip in its popover, drawn as TipKit draws a popover tip: the popover's
/// own background, at most 400 points wide. A touch beside it reaches the
/// guest, as a touch beside TipKit's does.
private struct SideKeysTipCard: View {
    let tip: SideKeysTip

    var body: some View {
        TipView(tip)
            .tipBackground(.clear)
            .frame(idealWidth: 400, maxWidth: 400)
            .fixedSize(horizontal: false, vertical: true)
            .presentationCompactAdaptation(.popover)
            .presentationBackgroundInteraction(.enabled)
    }
}
