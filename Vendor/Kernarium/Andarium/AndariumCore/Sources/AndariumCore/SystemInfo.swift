// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The system Andarium carries, as the Info screen and the boot screens name
/// it. The live app reads it from the release manifest the packaging step
/// stages beside the system image.
public struct SystemInfo: Equatable, Sendable {
    /// The system's version number as the screens say it ("17").
    public var version: String
    /// The release identifier, which names this build everywhere. A screen
    /// shows its `ReleaseLabel` instead; the ID itself is what reports,
    /// records and logs carry.
    public var release: String
    /// The system's own build identifier.
    public var buildID: String?
    /// The security patch level as the Info screen words it ("1 October
    /// 2026"); shown only where the manifest has one.
    public var securityPatch: String?
    public var kernel: String
    public var architecture: String
    /// The application binary interfaces the image enables ("arm64-v8a",
    /// "armeabi-v7a", "armeabi"), as the release records them
    /// (`android.abi_list`); empty for a release that records none.
    public var abis: [String]
    public var appVersion: String

    public init(
        version: String, release: String, buildID: String? = nil,
        securityPatch: String? = nil, kernel: String, architecture: String,
        abis: [String] = [], appVersion: String
    ) {
        self.version = version
        self.release = release
        self.buildID = buildID
        self.securityPatch = securityPatch
        self.kernel = kernel
        self.architecture = architecture
        self.abis = abis
        self.appVersion = appVersion
    }

    /// "2026-10-01" as "1 October 2026"; nil for anything else.
    static func patchLevel(_ text: String) -> String? {
        let parts = text.split(separator: "-", omittingEmptySubsequences: false).map { Int($0) }
        guard parts.count == 3, let year = parts[0], let month = parts[1], let day = parts[2],
              (1...12).contains(month), (1...31).contains(day), year >= 2000
        else { return nil }
        let names = ["January", "February", "March", "April", "May", "June", "July", "August", "September",
                     "October", "November", "December"]
        return "\(day) \(names[month - 1]) \(year)"
    }

    public enum ManifestError: Error, Equatable {
        case malformed
        case missing(String)
    }

    /// Reads a `release-manifest.json`.
    public init(manifest: Data, appVersion: String) throws {
        guard let root = try? JSONSerialization.jsonObject(with: manifest) as? [String: Any] else {
            throw ManifestError.malformed
        }
        func string(_ object: [String: Any]?, _ key: String, _ path: String) throws -> String {
            guard let value = object?[key] as? String, !value.isEmpty else { throw ManifestError.missing(path) }
            return value
        }
        let android = root["android"] as? [String: Any]
        let kernel = root["kernel"] as? [String: Any]
        let family = try string(root, "product_family", "product_family")
        let release = try string(root, "release_id", "release_id")
        guard let major = android?["major"] as? Int else { throw ManifestError.missing("android.major") }
        let kernelRelease = try string(kernel, "release", "kernel.release")
        guard let pageSize = kernel?["page_size"] as? Int, pageSize > 0 else {
            throw ManifestError.missing("kernel.page_size")
        }

        // The family "lineage-24-mainline-6.18-4k" is the one release line the app carries.
        let parts = family.split(separator: "-")
        guard parts.count >= 2, parts[0] == "lineage", Int(parts[1]) != nil else {
            throw ManifestError.missing("product_family")
        }
        // "6.18.54-g4bcc3653b86a" is Linux 6.18.54.
        let version = kernelRelease.split(separator: "-", maxSplits: 1).first.map(String.init) ?? kernelRelease

        self.init(
            version: String(major), release: release,
            buildID: android?["build_id"] as? String,
            securityPatch: (android?["security_patch"] as? String).flatMap(Self.patchLevel),
            kernel: "Linux \(version)", architecture: "arm64, \(pageSize / 1024) KiB pages",
            abis: (android?["abi_list"] as? [Any])?.compactMap { $0 as? String }.filter { !$0.isEmpty } ?? [],
            appVersion: appVersion
        )
    }
}
