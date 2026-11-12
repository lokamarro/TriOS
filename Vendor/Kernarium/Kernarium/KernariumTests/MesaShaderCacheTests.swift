// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing
@testable import Kernarium

/// The tests change the process environment, which Mesa and the rest of the
/// application read, so they run one at a time and put it back.
@Suite(.serialized)
struct MesaShaderCacheTests {
    private static let names = [
        "MESA_SHADER_CACHE_DISABLE",
        "MESA_SHADER_CACHE_DIR",
        "MESA_SHADER_CACHE_MAX_SIZE",
        "MESA_DISK_CACHE_DATABASE",
    ]

    /// Runs `body` with the Mesa variables cleared, then restores them.
    private static func withCleanEnvironment(_ body: () throws -> Void) rethrows {
        let saved = names.map { name in getenv(name).map { String(cString: $0) } }
        names.forEach { unsetenv($0) }
        defer {
            for (name, value) in zip(names, saved) {
                if let value { setenv(name, value, 1) } else { unsetenv(name) }
            }
        }
        try body()
    }

    private static func value(_ name: String) -> String? {
        getenv(name).map { String(cString: $0) }
    }

    @Test
    func cacheLivesBelowTheBundleIdentifier() {
        let root = URL(fileURLWithPath: "/var/mobile/Caches", isDirectory: true)
        let directory = MesaShaderCache.directory(
            userCacheDirectory: root,
            bundleIdentifier: "com.example.Guest"
        )
        #expect(directory.path == "/var/mobile/Caches/com.example.Guest")
    }

    @Test
    func theVariablesPlaceAndBoundTheCache() {
        let variables = MesaShaderCache.variables(
            directory: URL(fileURLWithPath: "/cache/com.example.Guest", isDirectory: true)
        )
        let named = Dictionary(uniqueKeysWithValues: variables.map { ($0.name, $0.value) })
        #expect(named["MESA_SHADER_CACHE_DIR"] == "/cache/com.example.Guest")
        #expect(named["MESA_SHADER_CACHE_MAX_SIZE"] == "\(MesaShaderCache.limitMiB)M")
        #expect((named["MESA_DISK_CACHE_DATABASE"] != nil) == MesaShaderCache.usesDatabase)
    }

    /// Mesa creates only the last component of the path it is given.
    @Test
    func configuringCreatesTheDirectoryAndSetsTheVariables() throws {
        let root = try makeScratchDirectory("mesa-shader-cache")
        defer { try? FileManager.default.removeItem(at: root) }
        let directory = root.appendingPathComponent("Caches/com.example.Guest", isDirectory: true)
        Self.withCleanEnvironment {
            #expect(MesaShaderCache.configure(directory: directory) == .configured(directory))
            #expect(Self.value("MESA_SHADER_CACHE_DIR") == directory.path)
            #expect(Self.value("MESA_SHADER_CACHE_MAX_SIZE") == "\(MesaShaderCache.limitMiB)M")
            #expect(Self.value("MESA_SHADER_CACHE_DISABLE") == nil)
        }
        var isDirectory: ObjCBool = false
        #expect(FileManager.default.fileExists(atPath: directory.path, isDirectory: &isDirectory))
        #expect(isDirectory.boolValue)
    }

    /// A harness that names its own cache keeps it.
    @Test
    func aVariableTheEnvironmentHasStaysAsItIs() throws {
        let root = try makeScratchDirectory("mesa-shader-cache")
        defer { try? FileManager.default.removeItem(at: root) }
        let directory = root.appendingPathComponent("unused", isDirectory: true)
        Self.withCleanEnvironment {
            setenv("MESA_SHADER_CACHE_DIR", "/named/by/the/caller", 1)
            setenv("MESA_SHADER_CACHE_MAX_SIZE", "17M", 1)
            #expect(MesaShaderCache.configure(directory: directory) == .configured(directory))
            #expect(Self.value("MESA_SHADER_CACHE_DIR") == "/named/by/the/caller")
            #expect(Self.value("MESA_SHADER_CACHE_MAX_SIZE") == "17M")
        }
        #expect(!FileManager.default.fileExists(atPath: directory.path))
    }

    @Test
    func aCacheTheCallerDisabledStaysDisabled() throws {
        let root = try makeScratchDirectory("mesa-shader-cache")
        defer { try? FileManager.default.removeItem(at: root) }
        let directory = root.appendingPathComponent("Caches/com.example.Guest", isDirectory: true)
        Self.withCleanEnvironment {
            setenv("MESA_SHADER_CACHE_DISABLE", "true", 1)
            #expect(MesaShaderCache.configure(directory: directory) == .leftToTheCaller)
            #expect(Self.value("MESA_SHADER_CACHE_DIR") == nil)
            #expect(Self.value("MESA_SHADER_CACHE_MAX_SIZE") == nil)
        }
        #expect(!FileManager.default.fileExists(atPath: directory.path))
    }

    /// The cache must not fall back to `$HOME/.cache`.
    @Test
    func aDirectoryThatCannotBeCreatedDisablesTheCache() throws {
        let root = try makeScratchDirectory("mesa-shader-cache")
        defer { try? FileManager.default.removeItem(at: root) }
        let file = root.appendingPathComponent("file")
        try Data("x".utf8).write(to: file)
        Self.withCleanEnvironment {
            let outcome = MesaShaderCache.configure(
                directory: file.appendingPathComponent("Caches", isDirectory: true)
            )
            #expect(outcome == .unavailable)
            #expect(Self.value("MESA_SHADER_CACHE_DISABLE") == "true")
            #expect(Self.value("MESA_SHADER_CACHE_DIR") == nil)
        }
    }

    /// The application ran `configureBeforeFirstUse()` from its launch, before
    /// this test: its process either was told to run without the cache or
    /// holds the variables, naming a directory it created in its own cache
    /// directory.
    @Test
    func theLaunchPlacedTheCacheInTheApplicationsCacheDirectory() throws {
        if Self.value("MESA_SHADER_CACHE_DISABLE") != nil { return }
        let directory = try #require(Self.value("MESA_SHADER_CACHE_DIR"))
        let root = try #require(MetalShaderCache.userCacheDirectory)
        #expect(directory.hasPrefix(root.path))
        #expect(directory.hasSuffix(try #require(Bundle.main.bundleIdentifier)))
        #expect(FileManager.default.fileExists(atPath: directory))
        #expect(Self.value("MESA_SHADER_CACHE_MAX_SIZE") != nil)
    }
}
