// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// Gives Mesa's disk shader cache a home of its own and a size limit.
///
/// Mesa keeps a disk cache of the shaders and pipeline data Zink and
/// KosmicKrisp compile, and finds it through the process environment when its
/// screen is created. Left alone it would live below `$HOME/.cache`, which a
/// device container does not let Mesa create, so the cache was off on a device
/// and, on a Mac, shared with every program of the user. The application
/// places it beside Metal's cache, in the system's cache directory for this
/// application (`Library/Caches/<bundle identifier>` on a device), and bounds
/// it: Mesa evicts entries once the cache passes `limitMiB`. Entries are keyed
/// by the identity of the Mesa binaries that wrote them, so a rebuilt
/// framework never reads an older one's entries, which age out under the
/// limit.
///
/// The variables are process-wide and Mesa reads them on a runtime thread
/// during boot, while the terminal's libghostty copies the environment at its
/// first initialization; the application calls
/// ``configureBeforeFirstUse()`` once, from its launch, before anything else
/// runs. The macOS emulators do the same in
/// `darwin/device/gpu/mesa_shader_cache.hpp`; the two change together.
nonisolated enum MesaShaderCache {
    /// The cache's size limit, in MiB, and whether Mesa keeps it in its
    /// database format (`MESA_DISK_CACHE_DATABASE`, a few large files) or one
    /// file per entry.
    static let limitMiB = 64
    static let usesDatabase = false

    enum Outcome: Equatable {
        /// There is no cache directory: Mesa runs without a disk cache.
        case unavailable
        /// `MESA_SHADER_CACHE_DISABLE` was already set, so the caller chose.
        case leftToTheCaller
        /// The variables the environment did not have name the cache.
        case configured(URL)
    }

    /// Where Mesa places its `mesa_shader_cache` when
    /// `MESA_SHADER_CACHE_DIR` names `<user cache directory>/<bundle
    /// identifier>`, the same place Metal's cache is named after.
    static func directory(
        userCacheDirectory: URL,
        bundleIdentifier: String
    ) -> URL {
        userCacheDirectory
            .appendingPathComponent(bundleIdentifier, isDirectory: true)
    }

    /// The variables that place and bound the cache in `directory`.
    static func variables(directory: URL) -> [(name: String, value: String)] {
        var variables = [
            (name: "MESA_SHADER_CACHE_DIR", value: directory.path),
            (name: "MESA_SHADER_CACHE_MAX_SIZE", value: "\(limitMiB)M"),
        ]
        if usesDatabase {
            variables.append((name: "MESA_DISK_CACHE_DATABASE", value: "true"))
        }
        return variables
    }

    /// Creates `directory`, because Mesa creates only the last component of a
    /// path, and sets each variable the environment does not already have, so
    /// a harness or a test can choose its own cache. When the directory cannot
    /// be created the cache is disabled instead of falling back to
    /// `$HOME/.cache`.
    @discardableResult
    static func configure(
        directory: URL,
        fileManager: FileManager = .default
    ) -> Outcome {
        if getenv("MESA_SHADER_CACHE_DISABLE") != nil {
            return .leftToTheCaller
        }
        if getenv("MESA_SHADER_CACHE_DIR") == nil {
            do {
                try fileManager.createDirectory(
                    at: directory,
                    withIntermediateDirectories: true
                )
            } catch {
                print("Kernarium: cannot create the Mesa shader cache \(directory.path): \(error)")
                setenv("MESA_SHADER_CACHE_DISABLE", "true", 0)
                return .unavailable
            }
        }
        for variable in variables(directory: directory) {
            setenv(variable.name, variable.value, 0)
        }
        return .configured(directory)
    }

    /// Applies ``configure(directory:fileManager:)`` to this application's
    /// own cache directory. The application calls it once, from its launch,
    /// before any view or runtime exists.
    @discardableResult
    static func configureBeforeFirstUse() -> Outcome {
        guard let root = MetalShaderCache.userCacheDirectory,
              let bundleIdentifier = Bundle.main.bundleIdentifier
        else {
            setenv("MESA_SHADER_CACHE_DISABLE", "true", 0)
            return .unavailable
        }
        return configure(directory: directory(
            userCacheDirectory: root,
            bundleIdentifier: bundleIdentifier
        ))
    }
}
