// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What a bundle's payload boots. One runtime framework serves one flavour,
/// because its kernel is fixed in a signed carrier, so the flavour also selects
/// which framework the loader may open.
enum GuestFlavor: UInt32 {
    case linux = 0
    case android = 1
}

/// A guest bundle is a directory in Documents holding everything one guest
/// needs, including the writable disk it accumulates. The extension is neutral
/// about the operating system inside; the payload names the flavour.
struct GuestBundle: Equatable {
    static let pathExtension = "kern"

    /// Android rootfs payload layout: the release's files, which the runtime
    /// reads in place, and the instance's writable data disk, which it creates
    /// at the first launch. The signed runtime carries its kernel
    /// independently, so a kernel-only app update does not rewrite the bundle.
    enum Android {
        static let commandLine = "boot/kernel-command-line.txt"
        static let systemDisk = "guest/system.img"
        static let layout = "guest/partition-layout.json"
        static let dataDisk = "guest/data.img"
    }

    let url: URL
    let flavor: GuestFlavor

    var name: String { url.lastPathComponent }
    var commandLine: URL { url.appendingPathComponent(Android.commandLine) }
    var systemDisk: URL { url.appendingPathComponent(Android.systemDisk) }
    var layout: URL { url.appendingPathComponent(Android.layout) }
    /// The guest's only writable disk. Every launch reopens it, so guest state
    /// survives the app being closed and reopened. The runtime creates it when
    /// it is absent and only ever grows it.
    var dataDisk: URL { url.appendingPathComponent(Android.dataDisk) }

    /// Recognises a bundle from the inputs needed to launch it. The release
    /// manifest is provenance for people and tools, not a runtime identity.
    static func load(
        at url: URL,
        fileManager: FileManager = .default
    ) -> GuestBundle? {
        guard url.pathExtension.lowercased() == pathExtension,
              isDirectory(url, fileManager: fileManager)
        else {
            return nil
        }
        let bundle = GuestBundle(url: url, flavor: .android)
        guard isRegularFile(bundle.systemDisk, fileManager: fileManager),
              isRegularFile(bundle.layout, fileManager: fileManager),
              isRegularFile(bundle.commandLine, fileManager: fileManager)
        else {
            return nil
        }
        return bundle
    }

    private static func isDirectory(
        _ url: URL,
        fileManager: FileManager
    ) -> Bool {
        var directory: ObjCBool = false
        let exists = fileManager.fileExists(
            atPath: url.path,
            isDirectory: &directory
        )
        return exists && directory.boolValue
    }

    private static func isRegularFile(
        _ url: URL,
        fileManager: FileManager
    ) -> Bool {
        var directory: ObjCBool = false
        let exists = fileManager.fileExists(
            atPath: url.path,
            isDirectory: &directory
        )
        return exists && !directory.boolValue
    }

    static func availableNames(
        in directory: URL,
        fileManager: FileManager = .default
    ) -> [String] {
        guard let entries = try? fileManager.contentsOfDirectory(
            at: directory,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        ) else {
            return []
        }
        return entries.compactMap { entry in
            load(at: entry, fileManager: fileManager)?.name
        }.sorted {
            $0.localizedStandardCompare($1) == .orderedAscending
        }
    }
}
