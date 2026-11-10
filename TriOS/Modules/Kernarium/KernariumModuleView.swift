
import SwiftUI

/// Module Kernarium : reprend ContentView d'origine (affichage guest
/// MetalKit, console série, réglages) dans une fenêtre TriOS.
struct KernariumModuleView: View {
    @ObservedObject var instance: AppInstance
    @State private var tab = 0
    @State private var flavor = 0 // 0 = Linux, 1 = Android (Andarium)
    @State private var consoleText = "Kernarium — console série du noyau UML\nLe flavor choisi au premier démarrage est définitif pour la session TriOS (chargement unique par processus).\n"

    var body: some View {
        VStack(spacing: 0) {
            if !instance.adapter.isLinked { EngineNotLinkedBanner(app: .kernarium) }
            Picker("", selection: $tab) {
                Text("Écran guest").tag(0); Text("Console série").tag(1); Text("Réglages").tag(2); Text("Journal").tag(3)
            }
            .pickerStyle(.segmented).padding(12)

            switch tab {
            case 0: display
            case 1: console
            case 2: settings
            default: EngineLogView(instance: instance)
            }
        }
        .foregroundStyle(.white)
    }

    private var display: some View {
        VStack(spacing: 14) {
            ZStack {
                RoundedRectangle(cornerRadius: 12).fill(.black)
                if let kadapter = instance.adapter as? KernariumAdapter {
                    // La vraie surface Metal du guest : elle s'enregistre
                    // dans l'adaptateur, qui la passe au loader natif.
                    KernariumDisplayView(adapter: kadapter)
                        .clipShape(RoundedRectangle(cornerRadius: 12))
                }
                if !instance.engineState.isAlive {
                    VStack(spacing: 8) {
                        Image(systemName: "cpu.fill").font(.largeTitle).foregroundStyle(AppID.kernarium.accent)
                        Text(flavor == 0 ? "Linux (noyau UML 6.18)" : "Android 17 / LineageOS 24")
                            .font(.headline).foregroundStyle(.white.opacity(0.9))
                        Text(instance.engineState.label).font(.caption).foregroundStyle(.secondary)
                        Text("Surface Metal prête. GPU Vulkan→Metal via KosmicKrisp, OpenGL via Zink une fois le guest démarré.")
                            .font(.caption2).foregroundStyle(.secondary)
                            .multilineTextAlignment(.center).padding(.horizontal, 30)
                    }
                    .allowsHitTesting(false)
                }
            }
            .aspectRatio(16/10, contentMode: .fit)
            .padding(.horizontal, 16)
            .onChange(of: flavor) { _, f in
                (instance.adapter as? KernariumAdapter)?.flavor = f == 0 ? .linux : .android
            }
            HStack(spacing: 10) {
                Button { Task { await instance.startEngine() } } label: { Label("Démarrer", systemImage: "play.fill") }
                    .disabled(instance.engineState.isAlive)
                Button { Task { await instance.suspendEngine() } } label: { Label("Suspendre (quiesce)", systemImage: "pause.fill") }
                    .disabled(instance.engineState != .running)
                Button { Task { await instance.resumeEngine() } } label: { Label("Reprendre", systemImage: "playpause.fill") }
                    .disabled(instance.engineState != .suspended)
                Button { Task { await instance.stopEngine() } } label: { Label("Arrêter", systemImage: "stop.fill") }
                    .disabled(!instance.engineState.isAlive)
            }
            .buttonStyle(.borderedProminent).tint(AppID.kernarium.accent)
            Text("Kernarium est le seul des trois à savoir vraiment se suspendre : kernarium_runtime_loader_quiesce() met le guest en pause sans perdre sa RAM ni sa session.")
                .font(.caption2).foregroundStyle(.secondary).multilineTextAlignment(.center).padding(.horizontal, 24)
            Spacer()
        }
        .padding(.top, 8)
    }

    private var console: some View {
        VStack {
            TextEditor(text: $consoleText)
                .font(.system(.caption, design: .monospaced))
                .scrollContentBackground(.hidden).background(.black)
                .foregroundStyle(.green.opacity(0.9))
            Text("Console série du noyau (SerialConsoleView d'origine). En mode lié, la sortie arrive par KernariumConsoleOutputHandler.")
                .font(.caption2).foregroundStyle(.secondary).padding(6)
        }
        .padding(.horizontal, 12)
    }

    private var settings: some View {
        Form {
            Section("Invité") {
                Picker("Système invité", selection: $flavor) {
                    Text("Linux").tag(0); Text("Android (Andarium)").tag(1)
                }
                .disabled(instance.engineState.isAlive)
                Text("Verrouillé dès le premier démarrage : kernarium_runtime_loader_select ne peut être appelé qu'une fois par processus.")
                    .font(.caption2).foregroundStyle(.secondary)
            }
            Section("Architecture (d'après le README d'origine)") {
                Text("Noyau Linux mainline 6.18 compilé en User-Mode Linux arm64 natif ; userspace émulé façon iSH (interpréteur + JIT), SMP ; disques UBD, réseau libslirp, Docker et bureaux Linux possibles.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .scrollContentBackground(.hidden)
    }
}
