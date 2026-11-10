
import Foundation

/// Un Mac virtuel, modèle repris de `Machines.swift` / `Installer.swift`
/// de Henge : un dossier par machine, avec ROM, disque et stockage
/// auxiliaire (l'équivalent de ce que crée VZMacAuxiliaryStorage).
struct VirtualMac: Codable, Identifiable, Equatable {
    var id: UUID = UUID()
    var name: String = "Mon Mac"
    var macOSVersion: String = "Sequoia 15.6.1"
    var ramMB: Int = 4096
    var cpus: Int = 4
    var diskGB: Int = 64
    var ecid: UInt64 = UInt64.random(in: 1...UInt64.max)
    var seed: UInt64 = UInt64.random(in: 1...UInt64.max)

    var romFile: String  { "\(id.uuidString)/rom.bin" }
    var diskFile: String { "\(id.uuidString)/disk.img" }
    var auxFile: String  { "\(id.uuidString)/aux.bin" }
    var installed: Bool = false
}
