import Foundation

/// Emplacements de fichiers de TriOS.
///
/// Règle d'isolation : chaque application hébergée écrit dans SON
/// sous-dossier. Les trois moteurs d'origine écrivaient chacun à la
/// racine de Documents (disques de VM, rootfs, aux storage…) : sans
/// cette séparation, Henge et LinuxRunner peuvent se marcher dessus.
enum TriOSStorage {
    static var root: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("TriOS", isDirectory: true)
    }

    static func container(for app: AppID) -> URL {
        root.appendingPathComponent(app.name, isDirectory: true)
    }

    static func prepareContainers() {
        let fm = FileManager.default
        for app in AppID.allCases {
            try? fm.createDirectory(at: container(for: app), withIntermediateDirectories: true)
        }
        try? fm.createDirectory(at: root.appendingPathComponent("Shared", isDirectory: true),
                                withIntermediateDirectories: true)
    }

    /// Petit magasin JSON générique pour les données propres à l'interface
    /// de chaque application (liste de Macs, distributions installées…).
    static func load<T: Decodable>(_ type: T.Type, file: String, for app: AppID) -> T? {
        let url = container(for: app).appendingPathComponent(file)
        guard let data = try? Data(contentsOf: url) else { return nil }
        return try? JSONDecoder().decode(T.self, from: data)
    }

    static func save<T: Encodable>(_ value: T, file: String, for app: AppID) {
        let url = container(for: app).appendingPathComponent(file)
        if let data = try? JSONEncoder().encode(value) {
            try? data.write(to: url, options: .atomic)
        }
    }
}
