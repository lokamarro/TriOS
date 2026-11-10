
import SwiftUI

/// L'écran d'accueil façon OS : fond d'écran, barre de statut,
/// grille d'icônes, dock — et, par-dessus, les fenêtres des
/// applications ouvertes, le switcher et le centre de contrôle.
///
/// Principe du multitâche simultané : TOUTES les fenêtres ouvertes
/// restent montées dans le ZStack en permanence. Une fenêtre réduite
/// ou non focalisée est masquée (opacité / hit-testing), jamais
/// démontée : aucun moteur ne perd sa session en naviguant.
struct DesktopRootView: View {
    @EnvironmentObject private var kernel: OSKernel
    @State private var clock = Date()
    private let timer = Timer.publish(every: 20, on: .main, in: .common).autoconnect()

    var body: some View {
        ZStack {
            wallpaper
            VStack(spacing: 0) {
                statusBar
                iconGrid
                Spacer()
                if kernel.focusedApp == nil { memoryHint }
                dock
            }
            .opacity(kernel.focusedApp != nil && fullscreenFocused ? 0 : 1)

            // Fenêtres : une par application ouverte, empilées par zIndex.
            ForEach(AppID.allCases) { app in
                let inst = kernel.instance(app)
                if inst.windowOpen {
                    AppWindowView(app: app, instance: inst)
                        .zIndex(inst.zIndex)
                        .opacity(inst.minimized ? 0 : 1)
                        .allowsHitTesting(!inst.minimized && (kernel.focusedApp == app || !inst.fullscreen))
                        .scaleEffect(inst.minimized ? 0.7 : 1)
                        .animation(.spring(response: 0.32, dampingFraction: 0.85), value: inst.minimized)
                }
            }

            if kernel.showSwitcher {
                AppSwitcherView().zIndex(10_000)
            }
            if kernel.showControlCenter {
                ControlCenterView().zIndex(9_000)
            }
        }
        .onReceive(timer) { clock = $0 }
        .gesture(switcherGesture)
    }

    private var fullscreenFocused: Bool {
        guard let f = kernel.focusedApp else { return false }
        return kernel.instance(f).fullscreen
    }

    /// Glisser depuis le bas = switcher (comme le geste iPadOS),
    /// quand aucune fenêtre plein écran ne capte déjà le geste.
    private var switcherGesture: some Gesture {
        DragGesture(minimumDistance: 60)
            .onEnded { v in
                if v.translation.height < -60, v.startLocation.y > 500 {
                    kernel.showSwitcher = true
                }
            }
    }

    private var wallpaper: some View {
        LinearGradient(colors: [Color(red: 0.07, green: 0.10, blue: 0.28),
                                Color(red: 0.35, green: 0.12, blue: 0.55),
                                Color(red: 0.05, green: 0.35, blue: 0.45)],
                       startPoint: .topLeading, endPoint: .bottomTrailing)
            .ignoresSafeArea()
    }

    private var statusBar: some View {
        HStack {
            Text(clock, format: .dateTime.hour().minute())
                .fontWeight(.semibold)
            Spacer()
            Text("TriOS").font(.caption.weight(.bold)).opacity(0.85)
            Spacer()
            HStack(spacing: 6) {
                if !kernel.runningApps.isEmpty {
                    Text("\(kernel.runningApps.count) app\(kernel.runningApps.count > 1 ? "s" : "") active\(kernel.runningApps.count > 1 ? "s" : "")")
                        .font(.caption2)
                }
                Image(systemName: "wifi")
                Image(systemName: "battery.75percent")
            }
            .font(.caption)
        }
        .foregroundStyle(.white)
        .padding(.horizontal, 18).padding(.top, 8)
    }

    private var iconGrid: some View {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 84), spacing: 18)], spacing: 20) {
            ForEach(AppID.allCases) { app in
                Button { kernel.launch(app) } label: {
                    AppIconView(app: app, instance: kernel.instance(app))
                }
                .buttonStyle(.plain)
                .contextMenu {
                    Button("Ouvrir") { kernel.launch(app) }
                    if kernel.instance(app).engineState.isAlive {
                        Button("Suspendre") { Task { await kernel.instance(app).suspendEngine() } }
                        Button("Quitter", role: .destructive) { kernel.quit(app) }
                    }
                }
            }
        }
        .padding(.horizontal, 24).padding(.top, 26)
    }

    private var memoryHint: some View {
        let total = kernel.totalGuestRAMMB
        return Group {
            if total > 0 {
                Label("RAM invités estimée : \(total) Mo — iOS peut fermer TriOS entier si la limite de l'appareil est dépassée.",
                      systemImage: total >= 6144 ? "exclamationmark.triangle.fill" : "memorychip")
                    .font(.caption2)
                    .foregroundStyle(total >= 6144 ? .yellow : .white.opacity(0.8))
                    .padding(.horizontal, 14).padding(.vertical, 7)
                    .background(.black.opacity(0.3), in: Capsule())
                    .padding(.bottom, 8)
            }
        }
    }

    private var dock: some View {
        HStack(spacing: 16) {
            ForEach(AppID.allCases) { app in
                Button { kernel.restore(app) } label: {
                    AppIconView(app: app, size: 52, showLabel: false, instance: kernel.instance(app))
                }
                .buttonStyle(.plain)
            }
            Divider().frame(height: 44).overlay(.white.opacity(0.4))
            Button { kernel.showSwitcher = true } label: {
                Image(systemName: "square.grid.2x2.fill")
                    .font(.title2).foregroundStyle(.white)
                    .frame(width: 52, height: 52)
                    .background(.white.opacity(0.18), in: RoundedRectangle(cornerRadius: 13))
            }
            .accessibilityLabel("Gestionnaire de tâches")
            Button { kernel.showControlCenter = true } label: {
                Image(systemName: "gearshape.fill")
                    .font(.title2).foregroundStyle(.white)
                    .frame(width: 52, height: 52)
                    .background(.white.opacity(0.18), in: RoundedRectangle(cornerRadius: 13))
            }
            .accessibilityLabel("Réglages TriOS")
        }
        .padding(.horizontal, 16).padding(.vertical, 10)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 24, style: .continuous))
        .padding(.bottom, 12)
    }
}
