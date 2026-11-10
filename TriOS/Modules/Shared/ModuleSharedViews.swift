
import SwiftUI

/// Bandeau affiché quand l'interface d'un module est là mais que sa
/// bibliothèque native ne l'est pas encore. Il dit exactement quoi
/// lancer, au lieu de laisser un écran mort.
struct EngineNotLinkedBanner: View {
    let app: AppID
    var body: some View {
        Label {
            Text("Interface prête, moteur \(app.name) non lié. Sur ton Mac : ./Scripts/integrate.sh puis ./Scripts/build_engines.sh, puis active le flag \(app.rawValue.uppercased())_LINKED (Docs/BUILD.md, étape 2).")
                .font(.caption)
        } icon: {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
        }
        .padding(10)
        .background(Color.orange.opacity(0.15), in: RoundedRectangle(cornerRadius: 10))
        .padding(.horizontal, 12).padding(.top, 10)
    }
}

/// Journal d'un moteur, alimenté par AppInstance.log.
struct EngineLogView: View {
    @ObservedObject var instance: AppInstance
    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                VStack(alignment: .leading, spacing: 3) {
                    ForEach(Array(instance.log.enumerated()), id: \.offset) { i, line in
                        Text(line)
                            .font(.system(.caption2, design: .monospaced))
                            .foregroundStyle(.green.opacity(0.9))
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .id(i)
                    }
                }
                .padding(10)
            }
            .background(.black)
            .onChange(of: instance.log.count) { _, n in
                if n > 0 { proxy.scrollTo(n - 1, anchor: .bottom) }
            }
        }
    }
}
