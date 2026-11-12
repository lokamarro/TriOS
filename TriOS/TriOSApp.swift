import SwiftUI

/// Point d'entrée UNIQUE de l'application unifiée.
///
/// Pourquoi un seul `@main` ?
/// Les trois applications d'origine ont chacune le leur :
/// - Henge       : `HengeApp`      (SwiftUI, `ios/Henge/HengeApp.swift`)
/// - Kernarium   : `KernariumApp`  (SwiftUI, `Kernarium/Kernarium/KernariumApp.swift`)
/// - LinuxRunner : `AppDelegate`   (UIKit,   `app/AppDelegate.m`, fork d'iSH)
///
/// iOS n'autorise qu'un seul point d'entrée par processus. Les trois
/// originaux sont donc désactivés par le script d'intégration
/// (voir `Scripts/integrate.sh` et `Docs/PATCHES.md`) et remplacés par
/// celui-ci. Toute l'initialisation globale des moteurs doit avoir lieu
/// ici, AVANT la première vue (Kernarium, par exemple, configure ses
/// caches Metal/Mesa dans son `init()` d'App).
@main
struct TriOSApp: App {
    @StateObject private var kernel = OSKernel()

    #if LINUXRUNNER_LINKED
    // LinuxRunner/iSH pilote son runtime via l'UIApplicationDelegate :
    // LRRuntimeBridge.startRuntime() appelle -[AppDelegate
    // startLinuxRuntime]. TriOS fournit sa propre implémentation de
    // cette classe (Modules/LinuxRunner/Native/TriOSAppDelegate.m,
    // séquence de boot reprise de l'AppDelegate d'origine, sans son UI)
    // et la branche ici ; SwiftUI continue de gérer les scènes.
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var linuxRunnerDelegate
    #endif

    init() {
        // Dossiers de données séparés par application :
        // Documents/TriOS/Henge, Documents/TriOS/Kernarium,
        // Documents/TriOS/LinuxRunner. Chaque moteur garde ses fichiers,
        // aucun ne peut écraser ceux d'un autre.
        TriOSStorage.prepareContainers()
        // Les initialisations "process global" des moteurs liés
        // statiquement sont appelées depuis EngineBootstrap, dans un
        // ordre précis et une seule fois.
        EngineBootstrap.prepareProcess()
    }

    var body: some Scene {
        WindowGroup {
            DesktopRootView()
                .environmentObject(kernel)
                .preferredColorScheme(.dark)
        }
    }
}
