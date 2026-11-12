// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import UIKit

/// A run: Android's display fills the screen, and the app keeps only a pair of
/// side keys on the edge. Tapped, the keys swell in place into Power and a
/// volume rocker, with Settings and a Back and Home pair right beside them
/// as one small group.
struct SessionView: View {
    @Environment(AppModel.self) private var model
    let mode: StartMode

    @State private var settings = false
    @State private var powerMenu = false
    @State private var screen: GuestScreen = .home
    @State private var volume = 9
    /// Android's own volume panel, shown after a volume key press.
    @State private var volumePanel = false
    @State private var volumeToken = 0
    @State private var recovery = RecoveryState()
    @State private var prepared = false

    var body: some View {
        @Bindable var model = model
        let guestModel = model.guest ?? Guest.fresh(on: model.host)
        ZStack {
            Color.black.ignoresSafeArea()
            guest
                .clipped()
                // As on the device, a touch on the guest closes the keys and
                // still reaches the guest.
                .simultaneousGesture(DragGesture(minimumDistance: 0).onChanged { _ in model.session.closeSideKeys() })
        }
        .overlay(alignment: .top) {
            if model.session.showStats {
                StatsPill(readout: model.session.readout, memoryMiB: guestModel.memoryMiB, recovery: mode == .recovery)
                    .transition(.move(edge: .top).combined(with: .opacity))
                    .padding(.top, StatsPlacement.top)
                    .allowsHitTesting(false)
            }
        }
        .overlay(alignment: .top) {
            GraphicsNoticeOverlay()
        }
        .overlay {
            SideKeysOverlay(
                mode: mode, lit: screen != .off,
                open: $model.session.sideKeysOpen, leading: model.sessionPreset.leftEdge,
                actions: SideKeyActions(
                    power: pressPower, powerHold: holdPower,
                    volumeUp: { pressVolume(up: true) }, volumeDown: { pressVolume(up: false) },
                    back: pressBack, home: pressHome,
                    settings: { settings = true }
                )
            )
        }
        .statusBarHidden(true)
        // The system honours the bottom edge's deferral only while the home
        // indicator is visible: with it hidden, one swipe goes Home.
        .persistentSystemOverlays(.visible)
        .sheet(isPresented: $settings) {
            SessionSettingsSheet(large: model.sessionPreset.settingsLarge)
                .environment(model)
        }
        // The screen over the session says why the guest is paused, and a
        // sheet would cover it.
        .onChange(of: model.spacePause != nil) { _, paused in
            if paused { settings = false }
        }
        .onAppear(perform: prepare)
        .task {
            while !Task.isCancelled {
                model.session.readout.tick(memoryMiB: guestModel.memoryMiB)
                try? await Task.sleep(for: .seconds(1))
            }
        }
    }

    // MARK: Guest

    @ViewBuilder
    private var guest: some View {
        ZStack {
            if mode == .recovery {
                GuestRecoveryScreen(state: $recovery, choose: chooseRecovery)
            } else {
                switch screen {
                case .home:
                    GuestHomeScreen(onSearch: { withAnimation(.easeOut(duration: 0.2)) { screen = .search } })
                case .search:
                    GuestSearchScreen(onBack: { pressBack() }, onHome: { pressHome() })
                case .setup:
                    GuestSetupScreen { withAnimation { screen = .home } }
                case .off:
                    Color.black
                }
                if volumePanel && screen != .off {
                    GuestVolumePanel(level: volume, maximum: 15)
                        .transition(.move(edge: .trailing).combined(with: .opacity))
                }
                if powerMenu {
                    GuestPowerMenu(
                        onPowerOff: { model.powerOff() },
                        onRestart: { model.restart() },
                        onDismiss: { withAnimation { powerMenu = false } }
                    )
                    .transition(.opacity)
                }
            }
        }
    }

    // MARK: Keys

    private func pressPower() {
        if mode == .recovery {
            chooseRecovery(recovery.highlighted)
            return
        }
        withAnimation(.easeInOut(duration: 0.25)) {
            powerMenu = false
            volumePanel = false
            screen = screen == .off ? .home : .off
        }
    }

    private func holdPower() {
        guard mode == .normal else { return }
        if screen == .off { screen = .home }
        withAnimation(.snappy) { powerMenu = true }
    }

    private func pressVolume(up: Bool) {
        if mode == .recovery {
            let count = recovery.items.count
            recovery.highlighted = (recovery.highlighted + (up ? count - 1 : 1)) % count
            return
        }
        // Android answers with its own volume panel, which it hides again
        // after a moment; while its screen is off, it changes the volume
        // without one.
        withAnimation(.snappy) {
            volume = min(15, max(0, volume + (up ? 1 : -1)))
            if screen != .off { volumePanel = true }
        }
        volumeToken += 1
        let token = volumeToken
        Task {
            try? await Task.sleep(for: .seconds(2.5))
            if token == volumeToken { withAnimation(.easeOut(duration: 0.25)) { volumePanel = false } }
        }
    }

    /// Android's Back: closes what is on top, as Android would. On the home
    /// screen Android does nothing, and neither does Andarium.
    private func pressBack() {
        guard screen != .off else { return }
        withAnimation(.easeOut(duration: 0.2)) {
            if volumePanel {
                volumePanel = false
            } else if powerMenu {
                powerMenu = false
            } else if screen == .search {
                screen = .home
            }
        }
    }

    /// Android's Home: back to the home screen from anywhere but setup.
    private func pressHome() {
        guard screen != .off else { return }
        withAnimation(.easeOut(duration: 0.2)) {
            volumePanel = false
            powerMenu = false
            if screen == .search { screen = .home }
        }
    }

    private func chooseRecovery(_ index: Int) {
        let items = recovery.items
        guard items.indices.contains(index) else { return }
        recovery.highlighted = index
        switch items[index].0 {
        case "Reboot system now": model.restart()
        case "Power off": model.powerOff()
        case "Factory reset": go(.factoryReset)
        case "Apply update": go(.update)
        case "Advanced": go(.advanced)
        case "Format data/factory reset": go(.confirm)
        case "Format data":
            model.factoryReset()
            go(.wiped)
        case "Back", "Cancel": go(.main)
        default: break
        }
    }

    private func go(_ page: RecoveryState.Page) {
        recovery.page = page
        recovery.highlighted = 0
    }

    private func prepare() {
        guard !prepared else { return }
        prepared = true
        let preset = model.sessionPreset
        screen = preset.guestScreen
        volumePanel = preset.volumePanel
        powerMenu = preset.powerMenu
        if preset.settings {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.45) { settings = true }
        }
    }
}
