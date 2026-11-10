
import SwiftUI

/// Réglages globaux de TriOS (pas les réglages internes de chaque app,
/// qui vivent dans leurs modules respectifs).
struct ControlCenterView: View {
    @EnvironmentObject private var kernel: OSKernel
    @AppStorage("trios.warnBeforeHeavyLaunch") private var warnHeavy = true

    var body: some View {
        ZStack {
            Color.black.opacity(0.5).ignoresSafeArea()
                .onTapGesture { kernel.showControlCenter = false }
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Text("Réglages TriOS").font(.title3.weight(.bold)).foregroundStyle(.white)
                    Spacer()
                    Button { kernel.showControlCenter = false } label: {
                        Image(systemName: "xmark.circle.fill").font(.title3)
                    }.foregroundStyle(.white.opacity(0.8))
                }
                Toggle("Avertir avant un lancement lourd (RAM cumulée)", isOn: $warnHeavy)
                    .foregroundStyle(.white)
                Divider().overlay(.white.opacity(0.2))
                ForEach(AppID.allCases) { app in
                    HStack {
                        Image(systemName: app.symbol).foregroundStyle(app.accent)
                        VStack(alignment: .leading) {
                            Text(app.name).foregroundStyle(.white)
                            Text("Licence \(app.license) • dossier : Documents/TriOS/\(app.name)")
                                .font(.caption2).foregroundStyle(.white.opacity(0.6))
                        }
                        Spacer()
                        Text(kernel.instance(app).adapter.isLinked ? "Moteur lié" : "Moteur non lié")
                            .font(.caption2.weight(.semibold))
                            .foregroundStyle(kernel.instance(app).adapter.isLinked ? .green : .orange)
                    }
                }
                Text("TriOS est un assemblage GPL : Henge et Kernarium sont en GPL-2.0, LinuxRunner en GPL-3.0. L'ensemble ne peut pas aller sur l'App Store ; installation par sideload (SideStore/AltStore/TrollStore) comme les apps d'origine.")
                    .font(.caption2).foregroundStyle(.white.opacity(0.6))
            }
            .padding(20)
            .frame(maxWidth: 520)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 20))
            .padding()
        }
    }
}
