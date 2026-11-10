
import SwiftUI
import UIKit

/// Contrôleurs UIKit d'origine de LinuxRunner, présentés dans la
/// fenêtre TriOS : `+[LRRuntimeBridge guiController]` (terminal GUI)
/// et `+[LRRuntimeBridge desktopController]` (bureau graphique).
/// Le terminal texte d'origine (TerminalViewController d'iSH) se
/// présente de la même façon depuis son propre module Vendor.
#if LINUXRUNNER_LINKED
struct LRDesktopControllerView: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> UIViewController {
        LRRuntimeBridge.desktopController()
    }
    func updateUIViewController(_ uiViewController: UIViewController, context: Context) {}
}

struct LRGUIControllerView: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> UIViewController {
        LRRuntimeBridge.guiController()
    }
    func updateUIViewController(_ uiViewController: UIViewController, context: Context) {}
}
#endif

/// Vue « Bureau » du module : le vrai contrôleur si le moteur est lié
/// et en cours, sinon l'explication de ce qui s'affichera là.
struct LinuxDesktopHostView: View {
    @ObservedObject var instance: AppInstance

    var body: some View {
        #if LINUXRUNNER_LINKED
        if instance.engineState.isAlive {
            LRDesktopControllerView().ignoresSafeArea(edges: .bottom)
        } else {
            placeholder
        }
        #else
        placeholder
        #endif
    }

    private var placeholder: some View {
        VStack(spacing: 10) {
            Image(systemName: "desktopcomputer").font(.largeTitle)
                .foregroundStyle(AppID.linuxRunner.accent)
            Text("Le bureau graphique LinuxRunner (Openbox/LXQt/KDE selon la distribution, rendu VNC/Metal d'origine) s'affiche ici, via LRRuntimeBridge.desktopController().")
                .font(.caption).foregroundStyle(.secondary)
                .multilineTextAlignment(.center).padding(.horizontal, 28)
            Text(instance.engineState.label).font(.caption2).foregroundStyle(.secondary)
        }
        .foregroundStyle(.white)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
