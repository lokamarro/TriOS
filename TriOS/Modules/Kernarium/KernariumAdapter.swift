import Foundation
import MetalKit

/// Adaptateur Kernarium — démarrage RÉEL, repris de la séquence
/// d'`EmulatorLauncher.start` + `runGuest` dans le `ContentView.swift`
/// d'origine :
///   1. kernarium_runtime_loader_select(flavor)   // une fois par processus
///   2. remplir un kernarium_runtime_boot (struct_size, taille, DPI,
///      mémoire, vCPU, chemins rootfs/overlay ou release Android)
///   3. kernarium_runtime_loader_run(mtkView, &boot)  // BLOQUANT :
///      lancé sur un thread d'arrière-plan, il ne revient que lorsque
///      le guest s'arrête ; `onGuestExit` prévient alors l'instance.
///
/// Prérequis fichiers (dossier du module) :
/// - Linux   : `rootfs.img` dans Documents/TriOS/Kernarium/
///   (image produite par l'outillage Kernarium d'origine) ;
/// - Android : dossier `android-release/` + disque `android-data.img`
///   au même endroit.
final class KernariumAdapter: EngineAdapter {
    let app: AppID = .kernarium

    #if KERNARIUM_LINKED
    var isLinked: Bool { true }
    #else
    var isLinked: Bool { false }
    #endif

    enum Flavor: UInt32 { case linux = 0, android = 1 }
    var flavor: Flavor = .linux
    var guestMemoryMiB: UInt32 = 2048
    var vCPUCount: UInt32 = 4

    /// La MTKView du guest, enregistrée par KernariumDisplayView.
    weak var metalView: MTKView?
    /// Appelé quand le run natif se termine (code de retour du guest).
    var onGuestExit: ((Int32) -> Void)?

    private var flavorSelected = false
    private var running = false

    func start(onLog: @escaping (String) -> Void) async -> Result<Void, EngineError> {
        #if KERNARIUM_LINKED
        guard let view = metalView else {
            return .failure(.native(code: -2, message: "Surface Metal absente : ouvre l'onglet Écran guest du module pour créer la vue."))
        }
        let container = TriOSStorage.container(for: .kernarium)
        let rootfs = container.appendingPathComponent("rootfs.img")
        let release = container.appendingPathComponent("android-release", isDirectory: true)
        switch flavor {
        case .linux:
            guard FileManager.default.fileExists(atPath: rootfs.path) else {
                return .failure(.native(code: -3, message: "rootfs.img manquant dans Documents/TriOS/Kernarium/. Produis-le avec l'outillage Kernarium d'origine (voir Docs/INTEGRATION_KERNARIUM.md)."))
            }
        case .android:
            guard FileManager.default.fileExists(atPath: release.path) else {
                return .failure(.native(code: -3, message: "Dossier android-release/ manquant dans Documents/TriOS/Kernarium/."))
            }
        }
        if !flavorSelected {
            let rc = kernarium_runtime_loader_select(flavor.rawValue)
            guard rc == 0 else {
                return .failure(.native(code: Int(rc), message: "kernarium_runtime_loader_select a échoué (le flavor est figé pour la vie du processus)."))
            }
            flavorSelected = true
            onLog("[Kernarium] Runtime chargé, flavor \(flavor == .linux ? "Linux" : "Android") figé pour cette session TriOS.")
        }
        guard !running else { return .success(()) }
        running = true

        let size = view.drawableSize
        let width = UInt32(max(size.width, 640))
        let height = UInt32(max(size.height, 480))
        let mem = guestMemoryMiB, cpus = vCPUCount, flv = flavor
        let rootfsPath = rootfs.path
        let overlayPath = container.appendingPathComponent("overlay.img").path
        let dataDiskPath = container.appendingPathComponent("android-data.img").path
        let releasePath = release.path
        let recordPath = container.appendingPathComponent("session-record.txt").path
        let timeZone = TimeZone.current.identifier as NSString

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            var boot = kernarium_runtime_boot()
            boot.struct_size = UInt32(MemoryLayout<kernarium_runtime_boot>.size)
            boot.width = width
            boot.height = height
            boot.dots_per_inch = 264
            boot.guest_memory_mib = mem
            boot.guest_processor_count = cpus
            boot.auto_resize_guest = 1
            boot.android_boot_mode = 0 // AUTO
            boot.android_data_gib = 16
            let rc: Int32 = rootfsPath.withCString { rootfsPtr in
                overlayPath.withCString { overlayPtr in
                    dataDiskPath.withCString { dataPtr in
                        releasePath.withCString { releasePtr in
                            recordPath.withCString { recordPtr in
                                switch flv {
                                case .linux:
                                    boot.rootfs_path = rootfsPtr
                                    boot.overlay_path = overlayPtr
                                case .android:
                                    boot.android_release_path = releasePtr
                                    boot.android_data_disk_path = dataPtr
                                }
                                boot.session_record_path = recordPtr
                                boot.android_time_zone = timeZone.utf8String
                                return withUnsafePointer(to: &boot) { ptr in
                                    kernarium_runtime_loader_run(view, ptr)
                                }
                            }
                        }
                    }
                }
            }
            self?.running = false
            onLog("[Kernarium] Le guest s'est arrêté (code \(rc)).")
            self?.onGuestExit?(rc)
        }
        onLog("[Kernarium] Guest \(flavor == .linux ? "Linux" : "Android") lancé sur la surface Metal (\(width)×\(height), \(mem) Mio, \(cpus) vCPU).")
        return .success(())
        #else
        return .failure(.notLinked)
        #endif
    }

    func stop() async {
        #if KERNARIUM_LINKED
        kernarium_runtime_loader_request_stop()
        #endif
    }

    /// Kernarium est le seul des trois à offrir une vraie suspension.
    func suspend() async {
        #if KERNARIUM_LINKED
        _ = kernarium_runtime_loader_quiesce()
        #endif
    }

    func resume() async {
        #if KERNARIUM_LINKED
        kernarium_runtime_loader_resume()
        #endif
    }
}
