
import Foundation
import SwiftUI

/// Une session vivante d'une application hébergée.
///
/// Un `AppInstance` est créé au premier lancement et n'est JAMAIS
/// détruit tant que son moteur n'est pas arrêté : sa vue reste dans
/// l'arbre, simplement masquée quand la fenêtre est réduite ou fermée.
/// C'est la même technique que Kernarium utilise déjà dans son
/// `ContentView` (opacité 0 + `allowsHitTesting(false)`) pour garder
/// son framebuffer et sa console en vie en changeant d'onglet.
///
/// Conséquence : ouvrir les trois applications, passer de l'une à
/// l'autre, les réduire dans le dock… ne redémarre aucun moteur.
final class AppInstance: ObservableObject, Identifiable {
    let app: AppID
    var id: AppID { app }

    @Published var engineState: EngineState = .stopped
    @Published var windowOpen: Bool = false
    @Published var minimized: Bool = false
    @Published var fullscreen: Bool = true
    /// Décalage de la fenêtre flottante (mode non plein écran, surtout iPad).
    @Published var windowOffset: CGSize = .zero
    @Published var zIndex: Double = 0
    @Published var startedAt: Date?
    @Published var log: [String] = []

    let adapter: EngineAdapter

    init(app: AppID, adapter: EngineAdapter) {
        self.app = app
        self.adapter = adapter
        if !adapter.isLinked { engineState = .engineNotLinked }
    }

    func appendLog(_ line: String) {
        DispatchQueue.main.async {
            self.log.append(line)
            if self.log.count > 400 { self.log.removeFirst(self.log.count - 400) }
        }
    }

    @MainActor
    func startEngine() async {
        guard engineState != .running, engineState != .starting else { return }
        engineState = .starting
        appendLog("[TriOS] Démarrage de \(app.name)…")
        let result = await adapter.start { [weak self] line in self?.appendLog(line) }
        switch result {
        case .success:
            engineState = .running
            startedAt = Date()
            appendLog("[TriOS] \(app.name) est en cours d'exécution.")
        case .failure(let error):
            if case .notLinked = error {
                engineState = .engineNotLinked
            } else {
                engineState = .failed(error.description)
            }
            appendLog("[TriOS] Échec : \(error.description)")
        }
    }

    @MainActor
    func stopEngine() async {
        await adapter.stop()
        engineState = adapter.isLinked ? .stopped : .engineNotLinked
        startedAt = nil
        appendLog("[TriOS] \(app.name) arrêté.")
    }

    @MainActor
    func suspendEngine() async {
        await adapter.suspend()
        if engineState == .running { engineState = .suspended }
    }

    @MainActor
    func resumeEngine() async {
        await adapter.resume()
        if engineState == .suspended { engineState = .running }
    }
}
