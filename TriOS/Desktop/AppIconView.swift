
import SwiftUI

/// Icône d'application : tuile arrondie dégradée + glyphe, point
/// d'activité en dessous quand le moteur vit (style dock macOS/iPadOS).
struct AppIconView: View {
    let app: AppID
    var size: CGFloat = 62
    var showLabel: Bool = true
    @ObservedObject var instance: AppInstance

    var body: some View {
        VStack(spacing: 5) {
            ZStack {
                RoundedRectangle(cornerRadius: size * 0.24, style: .continuous)
                    .fill(LinearGradient(colors: [app.accent, app.accent.opacity(0.55)],
                                         startPoint: .topLeading, endPoint: .bottomTrailing))
                    .shadow(color: .black.opacity(0.35), radius: 6, y: 3)
                Image(systemName: app.symbol)
                    .font(.system(size: size * 0.44, weight: .medium))
                    .foregroundStyle(.white)
                if instance.engineState == .starting {
                    ProgressView().tint(.white)
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
                        .padding(5)
                }
            }
            .frame(width: size, height: size)
            if showLabel {
                Text(app.name)
                    .font(.caption2.weight(.medium))
                    .foregroundStyle(.white)
                    .shadow(color: .black.opacity(0.6), radius: 2)
                    .lineLimit(1)
            }
            Circle()
                .fill(instance.engineState.isAlive ? Color.white : Color.clear)
                .frame(width: 5, height: 5)
        }
    }
}
