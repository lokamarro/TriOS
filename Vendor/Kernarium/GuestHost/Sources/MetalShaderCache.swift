// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// Keeps the Metal shader cache only while its index is small.
///
/// Metal keeps one shader cache per application and reads that cache's whole
/// index into the process the first time anything in it compiles a pipeline.
/// Kernarium's display and terminal compile theirs as the application starts,
/// so every entry an earlier run left behind is paid for in memory from the
/// first frame on, whether this boot uses it or not. What real guest workloads
/// leave is small and saves host compilation on the next boot; GPU
/// conformance campaigns fill it to about 2.2 GB. Metal has no call that clears,
/// bounds or bypasses the cache and does not say which entries came from a
/// guest shader, so the application measures the index before anything in
/// the process can open it and removes the whole cache once the index has
/// grown past `indexLimit`.
nonisolated enum MetalShaderCache {
    /// The index bytes the cache may hold and still be kept. A desktop boot,
    /// glmark2, WebGL Aquarium and a Steam boot together leave about 0.12 MB,
    /// and repeating them adds nothing. An index costs about 3.5 MiB per MB
    /// on an iPad mini 6 and 5 MiB per MB on an M1 Ultra, and one that Metal
    /// has let grow to its own ceiling, about 23 MB, costs 80 to 115 MiB. The
    /// macOS emulators bound their cache by the same limit
    /// (`darwin/device/gpu/metal_shader_cache.hpp`); the two change together.
    static let indexLimit: UInt64 = 1_000_000

    /// The bytes a cache holds: `index` in its `.list` files, which the
    /// first compile reads into memory, and `data` in everything else, which
    /// the process maps and pages in only as entries are used.
    struct Size: Equatable {
        var index: UInt64 = 0
        var data: UInt64 = 0
    }

    enum Outcome: Equatable {
        case absent
        case kept(Size)
        case removed(Size)
    }

    /// Metal keeps the cache in `com.apple.metal` below a directory named
    /// after the bundle identifier in the Darwin user cache directory. That
    /// layout is Metal's own and undocumented: if it changes, nothing is
    /// removed and the cache simply stays.
    static func directory(
        userCacheDirectory: URL,
        bundleIdentifier: String
    ) -> URL {
        userCacheDirectory
            .appendingPathComponent(bundleIdentifier, isDirectory: true)
            .appendingPathComponent("com.apple.metal", isDirectory: true)
    }

    /// The Darwin user cache directory. An iOS application on a Mac has one
    /// outside its container, which only `confstr(_CS_DARWIN_USER_CACHE_DIR)`
    /// names. On a device that query fails with `EIO`, and the directory is
    /// the container's `Library/Caches`.
    static var userCacheDirectory: URL? {
        darwinUserCacheDirectory ?? FileManager.default.urls(
            for: .cachesDirectory,
            in: .userDomainMask
        ).first
    }

    private static var darwinUserCacheDirectory: URL? {
        let length = confstr(_CS_DARWIN_USER_CACHE_DIR, nil, 0)
        guard length > 1 else { return nil }
        var path = [CChar](repeating: 0, count: length)
        guard confstr(_CS_DARWIN_USER_CACHE_DIR, &path, length) == length else {
            return nil
        }
        return URL(fileURLWithPath: String(cString: path), isDirectory: true)
    }

    /// The size of the cache at `directory`, or nil when there is none. The
    /// index files sit at different depths per platform (the GPU binaries'
    /// below a GPU-named directory on a Mac, directly in the cache on a
    /// device), so every `.list` file below the directory counts.
    static func size(
        at directory: URL,
        fileManager: FileManager = .default
    ) -> Size? {
        guard fileManager.fileExists(atPath: directory.path) else { return nil }
        var size = Size()
        let keys: [URLResourceKey] = [.isRegularFileKey, .fileSizeKey]
        if let files = fileManager.enumerator(
            at: directory,
            includingPropertiesForKeys: keys
        ) {
            for case let file as URL in files {
                let values = try? file.resourceValues(forKeys: Set(keys))
                guard values?.isRegularFile == true else { continue }
                let bytes = UInt64(values?.fileSize ?? 0)
                if file.pathExtension == "list" {
                    size.index += bytes
                } else {
                    size.data += bytes
                }
            }
        }
        return size
    }

    /// Removes the cache at `directory` when its index holds more than
    /// `indexLimit` bytes; a cache exactly at the limit is kept.
    static func removeIfBloated(
        at directory: URL,
        indexLimit: UInt64 = indexLimit,
        fileManager: FileManager = .default
    ) throws -> Outcome {
        guard let size = size(at: directory, fileManager: fileManager) else {
            return .absent
        }
        guard size.index > indexLimit else { return .kept(size) }
        try fileManager.removeItem(at: directory)
        return .removed(size)
    }

    /// Applies the limit to this application's cache. The application calls
    /// it once, from its initializer, before any view or runtime exists: once
    /// Metal has read the index, removing the files no longer returns its
    /// memory.
    static func removeIfBloatedBeforeFirstUse() {
        guard let root = userCacheDirectory,
              let bundleIdentifier = Bundle.main.bundleIdentifier
        else { return }
        let cache = directory(
            userCacheDirectory: root,
            bundleIdentifier: bundleIdentifier
        )
        let clock = ContinuousClock()
        let start = clock.now
        let limit = megabytes(indexLimit)
        do {
            switch try removeIfBloated(at: cache) {
            case .absent:
                // Metal creates the cache during the first launch, and again
                // during any launch that removed it, so finding none on a
                // later launch means the layout above has moved.
                print("Kernarium: no Metal shader cache at \(cache.path)")
            case .kept(let size):
                print(String(
                    format: "Kernarium: kept the Metal shader cache, index %.3f MB of %.1f MB, data %.1f MB",
                    megabytes(size.index),
                    limit,
                    megabytes(size.data)
                ))
            case .removed(let size):
                let elapsed = start.duration(to: clock.now)
                let milliseconds = Double(elapsed.components.seconds) * 1000 +
                    Double(elapsed.components.attoseconds) / 1e15
                print(String(
                    format: "Kernarium: removed the Metal shader cache, index %.1f MB over %.1f MB, data %.1f MB, in %.1f ms",
                    megabytes(size.index),
                    limit,
                    megabytes(size.data),
                    milliseconds
                ))
            }
        } catch {
            print("Kernarium: cannot remove the Metal shader cache: \(error)")
        }
    }

    private static func megabytes(_ bytes: UInt64) -> Double {
        Double(bytes) / 1e6
    }
}
