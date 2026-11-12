// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// Stand-ins for the guest's Metal display. Android draws its own status bar
/// and navigation bar inside it; sizes are in points, which Android uses as
/// its density-independent pixels at 160 dpi.
struct GuestHomeScreen: View {
    /// Tapping the search bar opens the launcher's search with Android's keyboard.
    var onSearch: () -> Void = {}

    var body: some View {
        GeometryReader { geo in
            let wide = geo.size.width > geo.size.height * 1.1
            ZStack {
                Image("GuestWallpaper")
                    .resizable()
                    .scaledToFill()
                    .frame(width: geo.size.width, height: geo.size.height)
                    .clipped()
                VStack(spacing: 0) {
                    AndroidStatusBar(time: "9:41")
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Friday, Oct 30")
                            .font(.system(size: wide ? 22 : 20, weight: .regular))
                        Text("Partly cloudy · 18°")
                            .font(.system(size: 14))
                            .opacity(0.85)
                    }
                    .foregroundStyle(.white)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 24)
                    .padding(.top, 18)
                    Spacer(minLength: 8)
                    VStack(spacing: wide ? 18 : 22) {
                        iconRow(["Gallery", "Files", "Clock", "Settings"])
                        iconRow(["Phone", "Messaging", "Browser", "Camera"], labels: false)
                        HStack(spacing: 10) {
                            Image(systemName: "magnifyingglass")
                            Text("Search")
                            Spacer()
                            Image(systemName: "mic")
                        }
                        .font(.system(size: 16))
                        .foregroundStyle(Color(hex: 0x1C3A40))
                        .padding(.horizontal, 18)
                        .frame(height: 48)
                        .background(Color(hex: 0xD3E9EC), in: Capsule())
                        .contentShape(Capsule())
                        .onTapGesture(perform: onSearch)
                        .padding(.horizontal, 6)
                    }
                    .frame(maxWidth: 520)
                    .padding(.horizontal, 18)
                    .padding(.bottom, 10)
                    AndroidNavBar()
                }
            }
        }
    }

    private func iconRow(_ names: [String], labels: Bool = true) -> some View {
        HStack(spacing: 0) {
            ForEach(names, id: \.self) { name in
                VStack(spacing: 6) {
                    Image(systemName: GuestHomeScreen.symbol(for: name))
                        .font(.system(size: 22, weight: .medium))
                        .foregroundStyle(Color(hex: 0x16444A))
                        .frame(width: 56, height: 56)
                        .background(Color(hex: 0xCDE8EA), in: Circle())
                    if labels {
                        Text(name)
                            .font(.system(size: 12))
                            .foregroundStyle(.white)
                            .shadow(color: .black.opacity(0.3), radius: 2)
                    }
                }
                .frame(maxWidth: .infinity)
            }
        }
    }

    static func symbol(for app: String) -> String {
        switch app {
        case "Gallery": "photo"
        case "Files": "folder"
        case "Clock": "clock"
        case "Settings": "gearshape"
        case "Phone": "phone"
        case "Messaging": "message"
        case "Browser": "globe"
        default: "camera"
        }
    }
}

struct AndroidStatusBar: View {
    let time: String
    /// Dark icons for a light screen.
    var dark = false

    var body: some View {
        HStack(spacing: 6) {
            Text(time).font(.system(size: 14, weight: .medium))
            Spacer()
            Image(systemName: "wifi")
            Image(systemName: "battery.100percent.bolt")
        }
        .font(.system(size: 13, weight: .medium))
        .foregroundStyle(dark ? Color(hex: 0x161D1D) : .white)
        .padding(.horizontal, 16)
        .frame(height: 28)
    }
}

/// Android's three-button navigation: back, home, recents. While the
/// keyboard is up, Back turns into a down arrow that hides it and a
/// keyboard switcher appears at the end.
struct AndroidNavBar: View {
    var color: Color = .white
    var keyboardUp = false
    var onBack: () -> Void = {}
    var onHome: () -> Void = {}

    var body: some View {
        ZStack {
            HStack {
                Spacer()
                Group {
                    if keyboardUp {
                        Image(systemName: "chevron.down")
                            .font(.system(size: 19, weight: .bold))
                            .foregroundStyle(color)
                    } else {
                        Triangle()
                            .fill(color)
                            .frame(width: 15, height: 17)
                    }
                }
                .frame(width: 48, height: 48)
                .contentShape(Rectangle())
                .onTapGesture(perform: onBack)
                Spacer()
                Circle().fill(color).frame(width: 17, height: 17)
                    .frame(width: 48, height: 48)
                    .contentShape(Rectangle())
                    .onTapGesture(perform: onHome)
                Spacer()
                RoundedRectangle(cornerRadius: 2).fill(color).frame(width: 15, height: 15)
                    .frame(width: 48, height: 48)
                Spacer()
            }
            .frame(maxWidth: 380)
            if keyboardUp {
                HStack {
                    Spacer()
                    Image(systemName: "keyboard")
                        .font(.system(size: 15, weight: .medium))
                        .foregroundStyle(color)
                        .padding(.trailing, 18)
                }
            }
        }
        .frame(height: 48)
        .frame(maxWidth: .infinity)
    }
}

/// The launcher's search after tapping its search bar: Android raises its own
/// keyboard, as it does for any text field.
struct GuestSearchScreen: View {
    var onBack: () -> Void = {}
    var onHome: () -> Void = {}

    private let surface = Color(hex: 0xF3F8F8)
    private let ink = Color(hex: 0x161D1D)

    var body: some View {
        ZStack {
            surface
            VStack(spacing: 0) {
                AndroidStatusBar(time: "9:41", dark: true)
                HStack(spacing: 12) {
                    Image(systemName: "magnifyingglass")
                    HStack(spacing: 1) {
                        Text("cl")
                        Rectangle()
                            .fill(Color(hex: 0x006A6A))
                            .frame(width: 2, height: 22)
                    }
                    Spacer()
                    Image(systemName: "xmark")
                }
                .font(.system(size: 17))
                .foregroundStyle(ink)
                .padding(.horizontal, 18)
                .frame(height: 52)
                .background(Color(hex: 0xDAE5E5), in: Capsule())
                .padding(.horizontal, 16)
                .padding(.top, 10)
                .frame(maxWidth: 600)

                Text("Apps")
                    .font(.system(size: 14, weight: .medium))
                    .foregroundStyle(Color(hex: 0x3F4948))
                    .frame(maxWidth: 600, alignment: .leading)
                    .padding(.horizontal, 28)
                    .padding(.top, 22)
                HStack(spacing: 0) {
                    ForEach(["Clock", "Calculator", "Calendar"], id: \.self) { name in
                        VStack(spacing: 6) {
                            Image(systemName: Self.symbol(for: name))
                                .font(.system(size: 22, weight: .medium))
                                .foregroundStyle(Color(hex: 0x16444A))
                                .frame(width: 56, height: 56)
                                .background(Color(hex: 0xCDE8EA), in: Circle())
                            Text(name)
                                .font(.system(size: 12))
                                .foregroundStyle(ink)
                        }
                        .frame(maxWidth: .infinity)
                    }
                    Color.clear.frame(maxWidth: .infinity)
                }
                .frame(maxWidth: 600)
                .padding(.horizontal, 12)
                .padding(.top, 12)
                Spacer(minLength: 0)
                AndroidKeyboard()
                AndroidNavBar(color: Color(hex: 0x2B3133), keyboardUp: true, onBack: onBack, onHome: onHome)
                    .background(Color(hex: 0xE0E8E8))
            }
        }
    }

    static func symbol(for app: String) -> String {
        switch app {
        case "Clock": "clock"
        case "Calculator": "plus.forwardslash.minus"
        default: "calendar"
        }
    }
}

/// A stand-in for the system keyboard LineageOS ships, in its light
/// Material theme: suggestions, QWERTY and the action key.
struct AndroidKeyboard: View {
    private let base = Color(hex: 0xE0E8E8)
    private let key = Color(hex: 0xFBFDFD)
    private let function = Color(hex: 0xC4D4D5)
    private let ink = Color(hex: 0x161D1D)
    private let action = Color(hex: 0x006A6A)

    var body: some View {
        GeometryReader { geo in
            let gap: CGFloat = 6
            let unit = (min(geo.size.width, 640) - 12 - gap * 9) / 10
            VStack(spacing: 10) {
                HStack(spacing: 0) {
                    suggestion("cl")
                    divider
                    suggestion("Clock", bold: true)
                    divider
                    suggestion("close")
                }
                .frame(height: 36)
                row("qwertyuiop", unit: unit, gap: gap)
                row("asdfghjkl", unit: unit, gap: gap)
                HStack(spacing: gap) {
                    special(Image(systemName: "shift"), width: unit * 1.5)
                    ForEach(Array("zxcvbnm"), id: \.self) { letter(String($0), width: unit) }
                    special(Image(systemName: "delete.left"), width: unit * 1.5)
                }
                HStack(spacing: gap) {
                    special(Text("?123").font(.system(size: 14, weight: .medium)), width: unit * 1.5)
                    letter(",", width: unit, fill: function)
                    RoundedRectangle(cornerRadius: 7)
                        .fill(key)
                        .frame(width: unit * 5 + gap * 4, height: 46)
                        .overlay(Text("English").font(.system(size: 13)).foregroundStyle(ink.opacity(0.6)))
                    letter(".", width: unit, fill: function)
                    Image(systemName: "magnifyingglass")
                        .font(.system(size: 17, weight: .semibold))
                        .foregroundStyle(.white)
                        .frame(width: unit * 1.5 + gap, height: 46)
                        .background(action, in: RoundedRectangle(cornerRadius: 23))
                }
            }
            .frame(maxWidth: 640)
            .frame(maxWidth: .infinity)
            .padding(.top, 4)
        }
        .frame(height: 270)
        .background(base)
        .accessibilityElement()
        .accessibilityLabel("Keyboard")
    }

    private var divider: some View {
        Rectangle().fill(ink.opacity(0.15)).frame(width: 1, height: 20)
    }

    private func suggestion(_ text: String, bold: Bool = false) -> some View {
        Text(text)
            .font(.system(size: 16, weight: bold ? .semibold : .regular))
            .foregroundStyle(ink)
            .frame(maxWidth: .infinity)
    }

    private func row(_ letters: String, unit: CGFloat, gap: CGFloat) -> some View {
        HStack(spacing: gap) {
            ForEach(Array(letters), id: \.self) { letter(String($0), width: unit) }
        }
    }

    private func letter(_ text: String, width: CGFloat, fill: Color? = nil) -> some View {
        Text(text)
            .font(.system(size: 21))
            .foregroundStyle(ink)
            .frame(width: width, height: 46)
            .background(fill ?? key, in: RoundedRectangle(cornerRadius: 7))
    }

    private func special<L: View>(_ label: L, width: CGFloat) -> some View {
        label
            .font(.system(size: 17, weight: .medium))
            .foregroundStyle(ink)
            .frame(width: width, height: 46)
            .background(function, in: RoundedRectangle(cornerRadius: 7))
    }
}

private nonisolated struct Triangle: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.minX, y: rect.midY))
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY))
        path.closeSubpath()
        return path
    }
}

/// Android's own volume panel, which Android raises on its right edge when a
/// volume key is pressed: the ringer mode, the media volume slider, and more
/// settings. Andarium adds nothing of its own.
struct GuestVolumePanel: View {
    let level: Int
    let maximum: Int

    private let container = Color(hex: 0xEEF4F4)
    private let track = Color(hex: 0xC2D5D6)
    private let active = Color(hex: 0x006A6A)
    private let ink = Color(hex: 0x1A2B2C)

    var body: some View {
        VStack(spacing: 10) {
            Image(systemName: "bell.fill")
                .font(.system(size: 17, weight: .medium))
                .foregroundStyle(Color(hex: 0x002022))
                .frame(width: 48, height: 48)
                .background(Color(hex: 0x9ED1D6), in: Circle())
            GeometryReader { geo in
                let filled = max(52, geo.size.height * CGFloat(level) / CGFloat(max(maximum, 1)))
                ZStack(alignment: .bottom) {
                    RoundedRectangle(cornerRadius: 20, style: .continuous)
                        .fill(track)
                    RoundedRectangle(cornerRadius: 20, style: .continuous)
                        .fill(active)
                        .frame(height: filled)
                        .overlay(alignment: .top) {
                            Capsule()
                                .fill(.white.opacity(0.9))
                                .frame(width: 22, height: 4)
                                .padding(.top, 8)
                        }
                    Image(systemName: level == 0 ? "speaker.slash.fill" : "music.note")
                        .font(.system(size: 16, weight: .semibold))
                        .foregroundStyle(.white)
                        .padding(.bottom, 16)
                }
            }
            .frame(width: 48, height: 196)
            Image(systemName: "ellipsis")
                .font(.system(size: 17, weight: .bold))
                .foregroundStyle(ink)
                .frame(width: 48, height: 36)
        }
        .padding(8)
        .background(container, in: RoundedRectangle(cornerRadius: 32, style: .continuous))
        .shadow(color: .black.opacity(0.18), radius: 12, y: 4)
        .padding(.trailing, 12)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .trailing)
        .allowsHitTesting(false)
        .accessibilityElement()
        .accessibilityLabel("Volume")
        .accessibilityValue("\(level) of \(maximum)")
    }
}

/// Android's first-run setup, which a new phone's first start opens on.
struct GuestSetupScreen: View {
    var onNext: () -> Void = {}

    var body: some View {
        ZStack {
            Color(hex: 0x111416)
            VStack(alignment: .leading, spacing: 0) {
                AndroidStatusBar(time: "9:41")
                VStack(alignment: .leading, spacing: 14) {
                    Image(systemName: "hand.wave")
                        .font(.system(size: 34, weight: .regular))
                        .foregroundStyle(Color(hex: 0x9ED1D6))
                        .padding(.top, 48)
                    Text("Welcome")
                        .font(.system(size: 36, weight: .regular))
                        .foregroundStyle(.white)
                    Text("Let\u{2019}s set up your device.")
                        .font(.system(size: 16))
                        .foregroundStyle(Color(hex: 0xC4C9CB))
                    HStack {
                        Image(systemName: "globe")
                        Text("English (United States)")
                        Spacer()
                        Image(systemName: "chevron.down")
                    }
                    .font(.system(size: 16))
                    .foregroundStyle(.white)
                    .padding(.horizontal, 16)
                    .frame(height: 52)
                    .background(Color(hex: 0x1F2528), in: RoundedRectangle(cornerRadius: 14))
                    .padding(.top, 24)
                    HStack {
                        Image(systemName: "figure.stand")
                        Text("Accessibility")
                        Spacer()
                    }
                    .font(.system(size: 16))
                    .foregroundStyle(.white)
                    .padding(.horizontal, 16)
                    .frame(height: 52)
                    .background(Color(hex: 0x1F2528), in: RoundedRectangle(cornerRadius: 14))
                }
                .padding(.horizontal, 28)
                .frame(maxWidth: 560, alignment: .leading)
                Spacer()
                HStack {
                    Spacer()
                    Button(action: onNext) {
                        Text("Next")
                            .font(.system(size: 15, weight: .medium))
                            .foregroundStyle(Color(hex: 0x00363B))
                            .padding(.horizontal, 26)
                            .frame(height: 42)
                            .background(Color(hex: 0x9ED1D6), in: Capsule())
                    }
                    .buttonStyle(.plain)
                }
                .padding(.horizontal, 24)
                .padding(.bottom, 18)
                AndroidNavBar()
            }
        }
    }
}

/// Where the stock recovery is: its page and highlighted item. The side keys
/// drive it as on a phone (volume moves, Power chooses) as well as touch.
struct RecoveryState: Equatable {
    enum Page { case main, factoryReset, confirm, wiped, advanced, update }
    var page: Page = .main
    var highlighted = 0

    var items: [(String, String, Bool)] {
        switch page {
        case .main:
            [("Reboot system now", "arrow.clockwise", false),
             ("Apply update", "arrow.down.circle", true),
             ("Factory reset", "arrow.counterclockwise", true),
             ("Advanced", "gearshape", true),
             ("Power off", "power", false)]
        case .factoryReset:
            [("Format data/factory reset", "trash", false),
             ("Format cache", "trash", false),
             ("Back", "chevron.left", false)]
        case .confirm:
            [("Format data", "checkmark", false),
             ("Cancel", "xmark", false)]
        case .wiped:
            [("Back", "chevron.left", false)]
        case .advanced:
            [("Reboot to recovery", "arrow.clockwise", false),
             ("View recovery logs", "doc.text", false),
             ("Back", "chevron.left", false)]
        case .update:
            [("Apply from ADB", "cable.connector", false),
             ("Back", "chevron.left", false)]
        }
    }

    var subtitle: String {
        switch page {
        case .main: "Choose an option"
        case .factoryReset: "Factory reset"
        case .confirm: "Format data? All apps and data on this device will be erased."
        case .wiped: "Data wipe complete."
        case .advanced: "Advanced"
        case .update: "Apply update"
        }
    }

    static func == (a: RecoveryState, b: RecoveryState) -> Bool {
        a.page == b.page && a.highlighted == b.highlighted
    }
}

/// The stock recovery, drawn on the same display.
struct GuestRecoveryScreen: View {
    @Binding var state: RecoveryState
    let choose: (Int) -> Void

    var body: some View {
        ZStack {
            Color.black
            VStack(alignment: .leading, spacing: 0) {
                VStack(alignment: .leading, spacing: 6) {
                    Text("Recovery")
                        .font(.system(size: 30, weight: .medium))
                        .foregroundStyle(.white)
                    Text("Version 24.0 (\(PreviewData.release))")
                        .font(.system(size: 13, design: .monospaced))
                        .foregroundStyle(Color(hex: 0x9AA3A6))
                    Text(state.subtitle)
                        .font(.system(size: 15))
                        .foregroundStyle(Color(hex: 0x80CBC4))
                        .padding(.top, 14)
                }
                .padding(.horizontal, 24)
                .padding(.top, 48)
                .padding(.bottom, 18)
                VStack(spacing: 0) {
                    ForEach(Array(state.items.enumerated()), id: \.offset) { index, item in
                        Button {
                            state.highlighted = index
                            choose(index)
                        } label: {
                            HStack(spacing: 16) {
                                Image(systemName: item.1)
                                    .frame(width: 24)
                                Text(item.0)
                                Spacer()
                                if item.2 {
                                    Image(systemName: "chevron.right").font(.system(size: 14))
                                }
                            }
                            .font(.system(size: 17))
                            .foregroundStyle(index == state.highlighted ? .black : .white)
                            .padding(.horizontal, 24)
                            .frame(height: 58)
                            .background(index == state.highlighted ? Color(hex: 0x80CBC4) : .clear)
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                    }
                }
                .frame(maxWidth: 640)
                Spacer()
            }
        }
    }
}

/// Android's power menu, opened by holding Power.
struct GuestPowerMenu: View {
    let onPowerOff: () -> Void
    let onRestart: () -> Void
    let onDismiss: () -> Void

    var body: some View {
        ZStack {
            Color.black.opacity(0.55)
                .onTapGesture(perform: onDismiss)
            VStack(spacing: 10) {
                HStack(spacing: 10) {
                    tile("Power off", "power", action: onPowerOff)
                    tile("Restart", "arrow.clockwise", action: onRestart)
                }
                HStack(spacing: 10) {
                    tile("Screenshot", "camera.viewfinder", action: onDismiss)
                    tile("Lockdown", "lock", action: onDismiss)
                }
            }
            .frame(maxWidth: 320)
            .padding(.horizontal, 24)
        }
    }

    private func tile(_ title: String, _ symbol: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            VStack(alignment: .leading, spacing: 18) {
                Image(systemName: symbol)
                    .font(.system(size: 22))
                    .foregroundStyle(Color(hex: 0x00363B))
                    .frame(width: 44, height: 44)
                    .background(Color(hex: 0x9ED1D6), in: Circle())
                Text(title).font(.system(size: 15, weight: .medium)).foregroundStyle(.white)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(16)
            .background(Color(hex: 0x1D2528), in: RoundedRectangle(cornerRadius: 24))
        }
        .buttonStyle(.plain)
    }
}

/// Colours Android draws with, and the icon colours of the app's mark; the
/// rest of the app's chrome uses system colours.
