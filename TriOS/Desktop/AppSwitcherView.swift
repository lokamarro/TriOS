
import SwiftUI

/// Gestionnaire de tâches : toutes les sessions vivantes en même
/// temps, avec reprise, suspension et arrêt individuel.
struct AppSwitcherView: View {
    @EnvironmentObject private var kernel: OSKernel

    var body: some View {
        ZStack {
            Color.black.opacity(0.55).ignoresSafeArea()
                .onTapGesture { kernel.showSwitcher = false }
            VStack(alignment: .leading, spacing: 16) {
                HStack {
                    Text("Gestionnaire de tâches").font(.title2.weight(.bold)).foregroundStyle(.white)
                    Spacer()
                    Button { kernel.showSwitcher = false } label: {
                        Image(systemName: "xmark.circle.fill").font(.title2)
                    }
                    .foregroundStyle(.white.opacity(0.8))
                }
                Text("Les trois applications peuvent vivre en même temps. Fermer une carte ici arrête réellement son moteur ; fermer une fenêtre ne le fait pas.")
                    .font(.caption).foregroundStyle(.white.opacity(0.75))
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 16) {
                        ForEach(AppID.allCases) { app in card(app) }
                    }
                }
                Label("RAM invités totale estimée : \(kernel.totalGuestRAMMB) Mo. Au-delà de la limite jetsam de l'appareil, iOS ferme tout TriOS d'un coup — suspends une app lourde avant d'en lancer une autre.",
                      systemImage: "memorychip")
                    .font(.caption2).foregroundStyle(.yellow.opacity(0.9))
            }
            .padding(22)
        }
    }

    private func card(_ app: AppID) -> some View {
        let inst = kernel.instance(app)
        return VStack(alignment: .leading, spacing: 10) {
            HStack {
                AppIconView(app: app, size: 40, showLabel: false, instance: inst)
                VStack(alignment: .leading) {
                    Text(app.name).font(.headline).foregroundStyle(.white)
                    Text(inst.engineState.label).font(.caption2).foregroundStyle(.white.opacity(0.7))
                }
                Spacer()
                if inst.engineState.isAlive {
                    Button(role: .destructive) { kernel.quit(app) } label: {
                        Image(systemName: "xmark.circle.fill")
                    }
                    .foregroundStyle(.red.opacity(0.9))
                }
            }
            RoundedRectangle(cornerRadius: 10)
                .fill(app.accent.opacity(0.25))
                .frame(height: 110)
                .overlay {
                    Image(systemName: app.symbol).font(.largeTitle).foregroundStyle(.white.opacity(0.6))
                }
            HStack(spacing: 8) {
                if inst.engineState.isAlive {
                    Button("Reprendre") { kernel.showSwitcher = false; kernel.restore(app) }
                    if inst.engineState == .running {
                        Button("Suspendre") { Task { await inst.suspendEngine() } }
                    } else if inst.engineState == .suspended {
                        Button("Réveiller") { Task { await inst.resumeEngine() } }
                    }
                } else {
                    Button("Lancer") { kernel.showSwitcher = false; kernel.launch(app) }
                }
            }
            .buttonStyle(.bordered).controlSize(.small).tint(.white)
            if let started = inst.startedAt, inst.engineState.isAlive {
                Text("Démarrée à \(started, format: .dateTime.hour().minute().second())")
                    .font(.caption2).foregroundStyle(.white.opacity(0.55))
            }
        }
        .padding(14)
        .frame(width: 250)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 16))
    }
}
