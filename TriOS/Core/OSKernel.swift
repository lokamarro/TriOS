
import Foundation
import SwiftUI

/// Le "noyau" de l'OS TriOS : registre des applications, fenêtres,
/// focus, multitâche. Toute l'interface (bureau, dock, switcher)
/// lit et écrit ici, nulle part ailleurs.
final class OSKernel: ObservableObject {
    @Published var instances: [AppID: AppInstance] = [:]
    @Published var focusedApp: AppID?
    @Published var showSwitcher: Bool = false
    @Published var showControlCenter: Bool = false
    private var zCounter: Double = 1

    init() {
        instances = [
            .henge:       AppInstance(app: .henge,       adapter: HengeAdapter()),
            .kernarium:   AppInstance(app: .kernarium,   adapter: KernariumAdapter()),
            .linuxRunner: AppInstance(app: .linuxRunner, adapter: LinuxRunnerAdapter()),
        ]
        // Le run Kernarium est bloquant et vit sur un thread natif :
        // quand le guest s'arrête tout seul, l'adaptateur prévient ici
        // pour que l'état affiché redevienne « arrêté ».
        if let kadapter = instances[.kernarium]?.adapter as? KernariumAdapter,
           let kinstance = instances[.kernarium] {
            kadapter.onGuestExit = { code in
                Task { @MainActor in
                    kinstance.engineState = code == 0 ? .stopped : .failed("Le guest s'est arrêté avec le code \(code)")
                    kinstance.startedAt = nil
                }
            }
        }
    }

    func instance(_ app: AppID) -> AppInstance { instances[app]! }

    /// RAM guest totale des moteurs vivants (estimation, pour l'alerte jetsam).
    var totalGuestRAMMB: Int {
        instances.values.filter { $0.engineState.isAlive }.map { $0.app.defaultGuestRAMMB }.reduce(0, +)
    }

    var runningApps: [AppInstance] {
        instances.values
            .filter { $0.engineState.isAlive || $0.windowOpen }
            .sorted { $0.zIndex < $1.zIndex }
    }

    /// Lancer une application depuis le bureau ou le dock.
    /// Si elle tourne déjà, on rouvre simplement sa fenêtre :
    /// aucun redémarrage, aucune perte de session.
    func launch(_ app: AppID) {
        let inst = instance(app)
        inst.windowOpen = true
        inst.minimized = false
        focus(app)
        if inst.engineState == .stopped {
            Task { await inst.startEngine() }
        } else if inst.engineState == .suspended {
            Task { await inst.resumeEngine() }
        }
    }

    func focus(_ app: AppID) {
        zCounter += 1
        instance(app).zIndex = zCounter
        focusedApp = app
    }

    /// Fermer la fenêtre != arrêter le moteur. Le point sous l'icône
    /// du dock reste allumé et la session continue en arrière-plan.
    func closeWindow(_ app: AppID) {
        instance(app).windowOpen = false
        if focusedApp == app { focusedApp = nil }
    }

    func minimize(_ app: AppID) {
        instance(app).minimized = true
        if focusedApp == app { focusedApp = nil }
    }

    func restore(_ app: AppID) {
        let inst = instance(app)
        inst.windowOpen = true
        inst.minimized = false
        focus(app)
    }

    /// Arrêt complet depuis le switcher ou le dock (appui long).
    func quit(_ app: AppID) {
        let inst = instance(app)
        Task { await inst.stopEngine() }
        inst.windowOpen = false
        inst.minimized = false
        if focusedApp == app { focusedApp = nil }
    }
}
