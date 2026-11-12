// SPDX-License-Identifier: GPL-2.0-or-later
import Darwin
import Foundation
import Testing

@testable import Andarium

/// Andarium runs the guest's Mesa on a runtime thread that reads the process
/// environment, so the application places and bounds Mesa's disk shader cache
/// from its launch (`GuestHost/Sources/MesaShaderCache.swift`).
struct MesaShaderCacheLaunchTests {
    private static func value(_ name: String) -> String? {
        getenv(name).map { String(cString: $0) }
    }

    /// The process either was told to run without the cache, or holds the
    /// variables, naming a directory the launch created in the application's
    /// own cache directory.
    @Test func theLaunchPlacedTheCacheInTheApplicationsCacheDirectory() throws {
        if Self.value("MESA_SHADER_CACHE_DISABLE") != nil { return }
        let directory = try #require(Self.value("MESA_SHADER_CACHE_DIR"))
        let root = try #require(MetalShaderCache.userCacheDirectory)
        #expect(directory.hasPrefix(root.path))
        #expect(directory.hasSuffix(try #require(Bundle.main.bundleIdentifier)))
        #expect(FileManager.default.fileExists(atPath: directory))
        #expect(Self.value("MESA_SHADER_CACHE_MAX_SIZE") != nil)
    }
}
