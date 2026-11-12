import Foundation
@main struct Tests {
    static func main() throws {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: folder) }
        let url = folder.appendingPathComponent("environments.json")
        let original = LinuxEnvironment(name: "Test Alpine", rootfsName: UUID().uuidString)
        try EnvironmentCatalog(environments: [original]).write(url)
        let restored = try EnvironmentCatalog.read(url)
        precondition(restored.environments[0].id == original.id)
        precondition(restored.environments[0].rootfsName == original.rootfsName)
        precondition(restored.environments[0].applications == original.applications)
        for bad in ["", ".", "..", "a/b", "a\\b", "nul\0"] { precondition(!EnvironmentCatalog.validRootName(bad)) }
        for app in [LinuxApplication(name: "bad", command: "relative"), LinuxApplication(name: "bad", command: "/bin/sh", arguments: ["x\0y"]), LinuxApplication(name: "bad", command: "/bin/sh", arguments: [String(repeating: "x", count: 4096)])] {
            do { try app.validate(); fatalError("accepted invalid command") } catch {}
        }
        var gui = LinuxApplication(name: "Qt", command: "/usr/bin/qt-test")
        gui.displayMode = .application; gui.environment = ["QT_QPA_PLATFORM": "xcb"]
        try gui.validate()
        var withGUI = original; withGUI.applications.append(gui)
        try EnvironmentCatalog(environments: [withGUI]).write(url)
        let guiRoundTrip = try EnvironmentCatalog.read(url); precondition(guiRoundTrip.environments[0].applications.last == gui)
        // Decode a schema-1 checkpoint created before the optional GUI fields.
        let oldEncoder = JSONEncoder(); var old = try JSONSerialization.jsonObject(with: oldEncoder.encode(EnvironmentCatalog(environments: [original]))) as! [String: Any]
        var oldEnvironments = old["environments"] as! [[String: Any]]
        for key in ["distributionID", "packageManager", "environmentVariables", "guiConfiguration"] { oldEnvironments[0].removeValue(forKey: key) }
        old["environments"] = oldEnvironments
        try JSONSerialization.data(withJSONObject: old).write(to: url)
        let oldRoundTrip = try EnvironmentCatalog.read(url); precondition(oldRoundTrip.environments[0].id == original.id)
        let unsupported = Data("{\"schemaVersion\":9,\"environments\":[]}".utf8)
        try unsupported.write(to: url)
        do { _ = try EnvironmentCatalog.read(url); fatalError("accepted unsupported schema") } catch {}
        let preserved = try Data(contentsOf: url)
        precondition(preserved == unsupported)
        print("PASS: metadata persistence, stable IDs, launcher round-trip, invalid paths/argv, schema rejection preserves file")
    }
}
