import Foundation

/// Le contrat que chaque moteur doit remplir pour vivre dans TriOS.
///
/// Implémentations :
/// - `HengeAdapter`       -> API C `henge_vm_*`  (HengeBridge.h)
/// - `KernariumAdapter`   -> API C `kernarium_runtime_loader_*`
/// - `LinuxRunnerAdapter` -> classe ObjC `LRRuntimeBridge`
///
/// Chaque adaptateur compile en deux modes :
/// - moteur lié   (flag de compilation HENGE_LINKED / KERNARIUM_LINKED /
///   LINUXRUNNER_LINKED, activé par Scripts/build_engines.sh) :
///   les appels partent vers le vrai moteur ;
/// - moteur absent (défaut, projet qui vient d'être cloné) :
///   `isLinked == false`, `start()` échoue proprement avec
///   `.engineNotLinked` et l'interface affiche la marche à suivre.
///   Le projet compile et se lance dans les deux modes.
protocol EngineAdapter: AnyObject {
    var app: AppID { get }
    var isLinked: Bool { get }
    func start(onLog: @escaping (String) -> Void) async -> Result<Void, EngineError>
    func stop() async
    /// Met le moteur en pause sans détruire sa session (quand le moteur
    /// le permet : Kernarium a un vrai quiesce, Henge non — voir les
    /// adaptateurs pour le détail honnête de chaque cas).
    func suspend() async
    func resume() async
}

enum EngineError: Error, CustomStringConvertible {
    case notLinked
    case native(code: Int, message: String)

    var description: String {
        switch self {
        case .notLinked:
            return "Bibliothèque du moteur absente. Lance Scripts/integrate.sh puis Scripts/build_engines.sh (Docs/BUILD.md, étape 2)."
        case .native(_, let message):
            return message
        }
    }
}

/// Initialisations globales à faire une fois par processus, avant les vues.
enum EngineBootstrap {
    static func prepareProcess() {
        #if KERNARIUM_LINKED
        // Équivalent de l'init() de KernariumApp d'origine :
        // les caches de shaders doivent être configurés avant la
        // première MTKView, sinon Mesa écrit dans un dossier interdit.
        // Appeler ici les équivalents liés (MetalShaderCache /
        // MesaShaderCache) du module Kernarium.
        #endif
        #if HENGE_LINKED
        // Équivalent de l'init() de HengeApp : journalisation, réglages
        // moteur et garde JIT doivent précéder tout boot de VM.
        #endif
        #if LINUXRUNNER_LINKED
        // LinuxRunner/iSH prépare son backend d'exécution au lancement
        // (LRRuntimeBridge.prepareExecutionBackendAtLaunch).
        #endif
    }
}
