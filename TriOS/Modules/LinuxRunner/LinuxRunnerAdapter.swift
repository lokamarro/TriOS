import Foundation

/// Adaptateur LinuxRunner — démarrage RÉEL via la classe Objective-C
/// d'origine `LRRuntimeBridge` (`app/LinuxRunner/RuntimeBridge.h`) :
///   +setNextRootName:  -> choisit l'environnement à booter
///   +startRuntime      -> démarre le runtime iSH/LinuxRunner (int, 0 = OK)
///   +isRunning / +runtimeStatus / +stopDesktopSession / +taskSnapshot
///
/// Le runtime reste un état global de processus (un seul rootfs actif),
/// exactement comme dans l'app d'origine ; les environnements, eux,
/// sont installés côte à côte (onglet Distributions du module).
final class LinuxRunnerAdapter: EngineAdapter {
    let app: AppID = .linuxRunner

    #if LINUXRUNNER_LINKED
    var isLinked: Bool { true }
    #else
    var isLinked: Bool { false }
    #endif

    var activeRootName: String = "Debian"

    func start(onLog: @escaping (String) -> Void) async -> Result<Void, EngineError> {
        #if LINUXRUNNER_LINKED
        LRRuntimeBridge.setNextRootName(activeRootName)
        let rc = LRRuntimeBridge.startRuntime()
        guard rc == 0 else {
            return .failure(.native(code: Int(rc),
                message: "startRuntime a renvoyé \(rc) — statut : \(LRRuntimeBridge.runtimeStatus())"))
        }
        onLog("[LinuxRunner] Runtime démarré, environnement « \(activeRootName) ». Statut : \(LRRuntimeBridge.runtimeStatus())")
        return .success(())
        #else
        return .failure(.notLinked)
        #endif
    }

    func stop() async {
        #if LINUXRUNNER_LINKED
        LRRuntimeBridge.stopDesktopSession()
        #endif
    }

    /// iSH/LinuxRunner ne suspend pas son runtime : les processus
    /// guest continuent tant que l'app vit. Suspendre = masquer.
    func suspend() async {}
    func resume() async {}

    /// Processus guest vivants (gestionnaire de tâches natif d'origine).
    var guestProcesses: [[String: Any]] {
        #if LINUXRUNNER_LINKED
        return (LRRuntimeBridge.taskSnapshot() as? [[String: Any]]) ?? []
        #else
        return []
        #endif
    }
}
