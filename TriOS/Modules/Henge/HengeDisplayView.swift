
import SwiftUI

/// Affichage et entrées du Mac virtuel, branchés sur l'API C d'origine.
///
/// Affichage : `henge_vm_snapshot(bgra, w, h)` — l'en-tête le définit
/// comme « a small picture of the display, BGRA ». TriOS l'interroge
/// ~4 fois par seconde : suffisant pour suivre le boot et l'état du
/// Mac, et parfait pour les miniatures du switcher. Le chemin plein
/// débit de l'app d'origine (CAMetalLayer via
/// `henge_vm_set_display_layer`, voir SessionView) peut remplacer ce
/// composant sans changer le reste : même source, autre tuyau.
///
/// Entrées : trackpad relatif (`henge_vm_mouse_move/button/scroll`)
/// et touches spéciales en codes Linux, repris de Input.swift
/// d'origine : Esc 1, Retour 14, Tab 15, Ctrl 29, Maj 42, Option 56,
/// flèches 103/105/106/108, Cmd 125.
struct HengeDisplayView: View {
    @ObservedObject var instance: AppInstance
    @State private var image: UIImage?
    @State private var poller: Task<Void, Never>?

    private let snapshotW = 720
    private let snapshotH = 450

    var body: some View {
        VStack(spacing: 10) {
            ZStack {
                RoundedRectangle(cornerRadius: 12).fill(.black)
                if let image {
                    Image(uiImage: image).resizable().scaledToFit()
                } else {
                    VStack(spacing: 8) {
                        Image(systemName: "applelogo").font(.largeTitle)
                        Text(instance.engineState.isAlive
                             ? "En attente du premier frame du Mac…"
                             : "Démarre le Mac pour voir son écran ici.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    .foregroundStyle(.white.opacity(0.85))
                }
            }
            .aspectRatio(16/10, contentMode: .fit)
            .contentShape(Rectangle())
            .gesture(trackpadGesture)
            .onTapGesture { tapClick() }

            // Barre de touches spéciales, comme au-dessus du clavier d'origine.
            HStack(spacing: 6) {
                keyButton("esc", code: 1)
                keyButton("tab", code: 15)
                keyButton("ctrl", code: 29)
                keyButton("opt", code: 56)
                keyButton("cmd", code: 125)
                keyButton("⌫", code: 14)
                Spacer()
                keyButton("←", code: 105); keyButton("↑", code: 103)
                keyButton("↓", code: 108); keyButton("→", code: 106)
            }
            .padding(.horizontal, 4)
        }
        .onAppear { startPolling() }
        .onDisappear { poller?.cancel() }
    }

    // MARK: - Trackpad

    private var trackpadGesture: some Gesture {
        DragGesture(minimumDistance: 1)
            .onChanged { v in
                #if HENGE_LINKED
                // Déplacement relatif, comme le trackpad d'origine.
                let dx = Int32(v.translation.width * 1.6)
                let dy = Int32(v.translation.height * 1.6)
                henge_vm_mouse_move(dx, dy)
                #endif
            }
    }

    private func tapClick() {
        #if HENGE_LINKED
        henge_vm_mouse_button(0, 1)
        henge_vm_mouse_button(0, 0)
        #endif
    }

    private func keyButton(_ label: String, code: UInt16) -> some View {
        Button(label) {
            #if HENGE_LINKED
            henge_vm_key(code, 1)
            henge_vm_key(code, 0)
            #endif
        }
        .font(.caption.weight(.semibold))
        .padding(.horizontal, 10).padding(.vertical, 6)
        .background(.white.opacity(0.14), in: RoundedRectangle(cornerRadius: 7))
        .foregroundStyle(.white)
    }

    // MARK: - Snapshot

    private func startPolling() {
        poller?.cancel()
        poller = Task {
            while !Task.isCancelled {
                #if HENGE_LINKED
                if instance.engineState.isAlive, let img = Self.snapshot(width: snapshotW, height: snapshotH) {
                    await MainActor.run { image = img }
                }
                #endif
                try? await Task.sleep(nanoseconds: 250_000_000)
            }
        }
    }

    #if HENGE_LINKED
    private static func snapshot(width: Int, height: Int) -> UIImage? {
        var buffer = [UInt8](repeating: 0, count: width * height * 4)
        let rc = buffer.withUnsafeMutableBufferPointer { ptr in
            henge_vm_snapshot(ptr.baseAddress, Int32(width), Int32(height))
        }
        guard rc == 0 else { return nil }
        let info = CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.premultipliedFirst.rawValue
        guard let provider = CGDataProvider(data: Data(buffer) as CFData),
              let cg = CGImage(width: width, height: height, bitsPerComponent: 8,
                               bitsPerPixel: 32, bytesPerRow: width * 4,
                               space: CGColorSpaceCreateDeviceRGB(),
                               bitmapInfo: CGBitmapInfo(rawValue: info),
                               provider: provider, decode: nil,
                               shouldInterpolate: false, intent: .defaultIntent)
        else { return nil }
        return UIImage(cgImage: cg)
    }
    #endif
}
