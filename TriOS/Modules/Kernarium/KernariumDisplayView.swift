
import SwiftUI
import MetalKit

/// La surface Metal du guest Kernarium.
///
/// C'est LA pièce qui manquait à l'adaptateur : `kernarium_runtime_loader_run`
/// exige la MTKView réelle du guest (dans l'app d'origine, c'est
/// `GuestDisplayView`/`GuestMetalView` de GuestHost qui la fournit à
/// `EmulatorLauncher`). Ici, une MTKView standard est créée une fois,
/// enregistrée dans l'adaptateur, et le démarrage peut l'utiliser.
/// Les raffinements d'origine (saisie clavier UIKeyInput, tactile
/// multi-contacts mappé sur 0...32767) se rebranchent sur cette même
/// vue en suivant GuestHost/Sources/GuestDisplay.swift et GuestInput.swift.
struct KernariumDisplayView: UIViewRepresentable {
    let adapter: KernariumAdapter

    func makeUIView(context: Context) -> MTKView {
        let view = MTKView()
        view.device = MTLCreateSystemDefaultDevice()
        view.colorPixelFormat = .bgra8Unorm
        view.enableSetNeedsDisplay = false
        view.isPaused = false
        adapter.metalView = view
        return view
    }

    func updateUIView(_ uiView: MTKView, context: Context) {}
}
