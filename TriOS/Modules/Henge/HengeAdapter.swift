
import Foundation

/// Adaptateur Henge.
///
/// API native d'origine (relevée dans `ios/Henge/HengeBridge.h` et
/// `ios/Henge/Native/henge-vm.h`) — fonctions C GLOBALES, une seule VM
/// par processus :
///   henge_vm_boot(rom, disk, aux, ecid, seed, ram_mb, cpus, err, errlen)
///   henge_vm_install(...) / henge_vm_state() -> 0 arrêtée, 1 en cours, 2 stoppée
///   henge_vm_stop(), henge_vm_snapshot(...), henge_vm_set_display_layer(...)
///
/// Conséquence assumée : une seule instance Henge à la fois, comme
/// dans l'app d'origine ("Only one Mac can start per launch").
final class HengeAdapter: EngineAdapter {
    let app: AppID = .henge

    #if HENGE_LINKED
    var isLinked: Bool { true }
    #else
    var isLinked: Bool { false }
    #endif

    /// Configuration retenue pour le prochain boot (écrite par HengeView).
    var ramMB: UInt64 = 4096
    var cpus: Int32 = 4
    var mac: VirtualMac?

    func start(onLog: @escaping (String) -> Void) async -> Result<Void, EngineError> {
        #if HENGE_LINKED
        guard let mac else {
            return .failure(.native(code: -1, message: "Aucun Mac sélectionné. Crée ou importe un Mac dans l'onglet Macs."))
        }
        return await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                // henge_vm_share / henge_vm_set_display doivent précéder le boot.
                let container = TriOSStorage.container(for: .henge)
                let shared = TriOSStorage.root.appendingPathComponent("Shared", isDirectory: true).path
                let socket = container.appendingPathComponent("share.sock").path
                let agent = Bundle.main.path(forResource: "henge-agent", ofType: "gz")
                henge_vm_share(shared, socket, agent)
                henge_vm_set_display(1440, 900)
                var err = [CChar](repeating: 0, count: 512)
                let base = TriOSStorage.container(for: .henge)
                let rom  = base.appendingPathComponent(mac.romFile).path
                let disk = base.appendingPathComponent(mac.diskFile).path
                let aux  = base.appendingPathComponent(mac.auxFile).path
                let rc = henge_vm_boot(rom, disk, aux, mac.ecid, mac.seed,
                                       self.ramMB, self.cpus, &err, err.count)
                if rc == 0 {
                    onLog("[Henge] VM démarrée (état natif : \(henge_vm_state()))")
                    continuation.resume(returning: .success(()))
                } else {
                    continuation.resume(returning: .failure(
                        .native(code: Int(rc), message: String(cString: err))))
                }
            }
        }
        #else
        return .failure(.notLinked)
        #endif
    }

    func stop() async {
        #if HENGE_LINKED
        henge_vm_stop()
        #endif
    }

    /// Henge n'a pas d'API pause : suspendre = ne rien faire et le
    /// signaler honnêtement. La VM continue, simplement masquée.
    func suspend() async {}
    func resume() async {}

    /// Installation de macOS depuis un IPSW, via `henge_vm_install`
    /// (boot ROM en DFU + restauration USB, comme l'original).
    /// Bloquant : à appeler depuis un contexte async. La progression
    /// se lit pendant l'appel avec `installProgress()`.
    func install(mac: VirtualMac, ipswPath: String, onLog: @escaping (String) -> Void) async -> Result<Void, EngineError> {
        #if HENGE_LINKED
        return await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                var err = [CChar](repeating: 0, count: 512)
                let base = TriOSStorage.container(for: .henge)
                let dir = base.appendingPathComponent(mac.id.uuidString, isDirectory: true)
                try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                let rom  = base.appendingPathComponent(mac.romFile).path
                let disk = base.appendingPathComponent(mac.diskFile).path
                let aux  = base.appendingPathComponent(mac.auxFile).path
                if henge_make_aux(aux) != 0 {
                    onLog("[Henge] Création du stockage auxiliaire en échec (déjà présent ?)")
                }
                let rc = henge_vm_install(rom, disk, aux, mac.ecid, ipswPath,
                                           dir.appendingPathComponent("work").path,
                                           UInt64(mac.ramMB), Int32(mac.cpus), &err, err.count)
                if rc == 0 {
                    continuation.resume(returning: .success(()))
                } else {
                    continuation.resume(returning: .failure(
                        .native(code: Int(rc), message: String(cString: err))))
                }
            }
        }
        #else
        return .failure(.notLinked)
        #endif
    }

    /// (étape, progression 0...1) de l'installation en cours.
    static func installProgress() -> (step: Int32, fraction: Double) {
        #if HENGE_LINKED
        return (henge_install_step(), henge_install_progress())
        #else
        return (-1, 0)
        #endif
    }
}
