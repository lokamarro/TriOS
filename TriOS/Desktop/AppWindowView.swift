
import SwiftUI

/// Fenêtre d'une application hébergée.
///
/// Les trois boutons ont des sens DIFFÉRENTS, c'est le cœur du modèle :
/// - fermer (rouge)  : ferme la fenêtre, le moteur CONTINUE ;
/// - réduire (jaune) : envoie la fenêtre dans le dock, moteur inchangé ;
/// - arrêter (dans la barre d'outils de chaque app) : stoppe le moteur.
struct AppWindowView: View {
    let app: AppID
    @ObservedObject var instance: AppInstance
    @EnvironmentObject private var kernel: OSKernel
    @GestureState private var drag: CGSize = .zero

    var body: some View {
        VStack(spacing: 0) {
            titleBar
            Divider().overlay(.white.opacity(0.15))
            // Le contenu du module. Il n'est créé qu'une fois et
            // survit à la fermeture/réouverture de la fenêtre.
            ModuleHostView(app: app, instance: instance)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background(Color(red: 0.09, green: 0.09, blue: 0.11))
        .clipShape(RoundedRectangle(cornerRadius: instance.fullscreen ? 0 : 18, style: .continuous))
        .overlay {
            if !instance.fullscreen {
                RoundedRectangle(cornerRadius: 18).stroke(.white.opacity(0.25))
            }
        }
        .padding(instance.fullscreen ? 0 : 14)
        .offset(x: instance.fullscreen ? 0 : instance.windowOffset.width + drag.width,
                y: instance.fullscreen ? 0 : instance.windowOffset.height + drag.height)
        .shadow(color: .black.opacity(instance.fullscreen ? 0 : 0.5), radius: 24, y: 10)
        .onTapGesture { kernel.focus(app) }
    }

    private var titleBar: some View {
        HStack(spacing: 10) {
            HStack(spacing: 8) {
                windowButton(color: .red, symbol: "xmark") { kernel.closeWindow(app) }
                windowButton(color: .yellow, symbol: "minus") { kernel.minimize(app) }
                windowButton(color: .green, symbol: instance.fullscreen ? "arrow.down.right.and.arrow.up.left" : "arrow.up.left.and.arrow.down.right") {
                    instance.fullscreen.toggle()
                }
            }
            Image(systemName: app.symbol).foregroundStyle(app.accent)
            Text(app.name).font(.headline).foregroundStyle(.white)
            statusPill
            Spacer()
            if instance.engineState.isAlive {
                Button {
                    Task { await instance.stopEngine() }
                } label: {
                    Label("Arrêter", systemImage: "stop.circle")
                        .font(.caption.weight(.semibold))
                }
                .buttonStyle(.bordered).tint(.red).controlSize(.small)
            }
            Button { kernel.showSwitcher = true } label: {
                Image(systemName: "square.grid.2x2")
            }
            .foregroundStyle(.white.opacity(0.85))
        }
        .padding(.horizontal, 14).padding(.vertical, 9)
        .background(.ultraThinMaterial)
        .gesture(
            DragGesture()
                .updating($drag) { v, state, _ in if !instance.fullscreen { state = v.translation } }
                .onEnded { v in
                    if !instance.fullscreen {
                        instance.windowOffset.width += v.translation.width
                        instance.windowOffset.height += v.translation.height
                    }
                }
        )
    }

    private var statusPill: some View {
        HStack(spacing: 5) {
            Circle()
                .fill(instance.engineState.isAlive ? Color.green : (instance.engineState == .engineNotLinked ? Color.orange : Color.gray))
                .frame(width: 7, height: 7)
            Text(instance.engineState.label).font(.caption2)
        }
        .foregroundStyle(.white.opacity(0.85))
        .padding(.horizontal, 8).padding(.vertical, 3)
        .background(.white.opacity(0.12), in: Capsule())
    }

    private func windowButton(color: Color, symbol: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 8, weight: .bold))
                .foregroundStyle(.black.opacity(0.55))
                .frame(width: 14, height: 14)
                .background(color, in: Circle())
        }
        .buttonStyle(.plain)
    }
}

/// Aiguillage vers la vue racine de chaque module.
struct ModuleHostView: View {
    let app: AppID
    @ObservedObject var instance: AppInstance

    var body: some View {
        switch app {
        case .henge:       HengeModuleView(instance: instance)
        case .kernarium:   KernariumModuleView(instance: instance)
        case .linuxRunner: LinuxRunnerModuleView(instance: instance)
        }
    }
}
