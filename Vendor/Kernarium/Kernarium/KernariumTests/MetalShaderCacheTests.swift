// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import Kernarium

struct MetalShaderCacheTests {
    private static let bundleIdentifier = "com.example.Guest"
    private static let limit: UInt64 = 1000

    private static func write(_ count: Int, to file: URL) throws {
        try FileManager.default.createDirectory(
            at: file.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        try Data(count: count).write(to: file)
    }

    private static func cache(in root: URL) -> URL {
        MetalShaderCache.directory(
            userCacheDirectory: root,
            bundleIdentifier: bundleIdentifier
        )
    }

    /// A cache laid out as on a Mac: the GPU binaries' pair below a GPU
    /// directory and the front end's pair below its own, with data files far
    /// larger than the index, which must not count toward the limit.
    private static func writeCache(
        at cache: URL,
        functionsIndex: Int,
        librariesIndex: Int
    ) throws {
        try write(5000, to: cache.appendingPathComponent("16777235_594/functions.data"))
        try write(functionsIndex, to: cache.appendingPathComponent("16777235_594/functions.list"))
        try write(3000, to: cache.appendingPathComponent("32024/libraries.data"))
        try write(librariesIndex, to: cache.appendingPathComponent("32024/libraries.list"))
    }

    @Test
    func cacheLivesBelowTheBundleIdentifier() {
        let root = URL(fileURLWithPath: "/var/mobile/Caches", isDirectory: true)
        let cache = MetalShaderCache.directory(
            userCacheDirectory: root,
            bundleIdentifier: Self.bundleIdentifier
        )
        #expect(cache.path == "/var/mobile/Caches/com.example.Guest/com.apple.metal")
    }

    @Test
    func userCacheDirectoryExists() throws {
        let root = try #require(MetalShaderCache.userCacheDirectory)
        var isDirectory: ObjCBool = false
        #expect(FileManager.default.fileExists(
            atPath: root.path,
            isDirectory: &isDirectory
        ))
        #expect(isDirectory.boolValue)
    }

    @Test
    func theIndexIsEveryListFileAtAnyDepth() throws {
        let root = try makeScratchDirectory("metal-shader-cache")
        defer { try? FileManager.default.removeItem(at: root) }
        let cache = Self.cache(in: root)
        // A device keeps the GPU binaries' pair directly in the cache.
        try Self.write(700, to: cache.appendingPathComponent("functions.list"))
        try Self.write(9000, to: cache.appendingPathComponent("functions.data"))
        try Self.write(300, to: cache.appendingPathComponent("32024/libraries.list"))
        try Self.write(4000, to: cache.appendingPathComponent("32024/libraries.data"))
        try Self.write(24, to: cache.appendingPathComponent("archiveUsage.db"))

        #expect(MetalShaderCache.size(at: cache) ==
            MetalShaderCache.Size(index: 1000, data: 13024))
    }

    @Test
    func aCacheBelowTheLimitIsKept() throws {
        let root = try makeScratchDirectory("metal-shader-cache")
        defer { try? FileManager.default.removeItem(at: root) }
        let cache = Self.cache(in: root)
        try Self.writeCache(at: cache, functionsIndex: 600, librariesIndex: 399)

        #expect(try MetalShaderCache.removeIfBloated(at: cache, indexLimit: Self.limit) ==
            .kept(MetalShaderCache.Size(index: 999, data: 8000)))
        #expect(FileManager.default.fileExists(atPath: cache.path))
    }

    @Test
    func aCacheAtTheLimitIsKept() throws {
        let root = try makeScratchDirectory("metal-shader-cache")
        defer { try? FileManager.default.removeItem(at: root) }
        let cache = Self.cache(in: root)
        try Self.writeCache(at: cache, functionsIndex: 600, librariesIndex: 400)

        #expect(try MetalShaderCache.removeIfBloated(at: cache, indexLimit: Self.limit) ==
            .kept(MetalShaderCache.Size(index: 1000, data: 8000)))
        #expect(FileManager.default.fileExists(atPath: cache.path))
    }

    @Test
    func aCacheAboveTheLimitIsRemovedAndItsSiblingsStay() throws {
        let root = try makeScratchDirectory("metal-shader-cache")
        defer { try? FileManager.default.removeItem(at: root) }
        let cache = Self.cache(in: root)
        try Self.writeCache(at: cache, functionsIndex: 600, librariesIndex: 401)
        let sibling = root.appendingPathComponent(
            "\(Self.bundleIdentifier)/com.apple.metalfe/modules.timestamp"
        )
        try Self.write(10, to: sibling)

        #expect(try MetalShaderCache.removeIfBloated(at: cache, indexLimit: Self.limit) ==
            .removed(MetalShaderCache.Size(index: 1001, data: 8000)))
        #expect(!FileManager.default.fileExists(atPath: cache.path))
        #expect(FileManager.default.fileExists(atPath: sibling.path))
    }

    @Test
    func theApplicationLimitDecidesByDefault() throws {
        let root = try makeScratchDirectory("metal-shader-cache")
        defer { try? FileManager.default.removeItem(at: root) }
        let cache = Self.cache(in: root)
        let limit = MetalShaderCache.indexLimit
        let libraries = Int(limit / 2)
        try Self.writeCache(
            at: cache,
            functionsIndex: Int(limit) - libraries,
            librariesIndex: libraries
        )
        #expect(try MetalShaderCache.removeIfBloated(at: cache) ==
            .kept(MetalShaderCache.Size(index: limit, data: 8000)))

        try Self.write(libraries + 1, to: cache.appendingPathComponent("32024/libraries.list"))
        #expect(try MetalShaderCache.removeIfBloated(at: cache) ==
            .removed(MetalShaderCache.Size(index: limit + 1, data: 8000)))
        #expect(!FileManager.default.fileExists(atPath: cache.path))
    }

    @Test
    func anAbsentCacheIsNotAnError() throws {
        let root = try makeScratchDirectory("metal-shader-cache")
        defer { try? FileManager.default.removeItem(at: root) }
        let cache = Self.cache(in: root)
        #expect(MetalShaderCache.size(at: cache) == nil)
        #expect(try MetalShaderCache.removeIfBloated(at: cache, indexLimit: Self.limit) ==
            .absent)
    }
}
