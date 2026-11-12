// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace uml::gpu {

/*
 * Mesa keeps a disk cache of the shaders and pipeline data Zink and
 * KosmicKrisp compile, and finds it through the process environment when its
 * screen is created, so the environment is the only place to place and bound
 * it. Left alone, the cache lands in `$HOME/.cache`, which a device container
 * does not let Mesa create and a Mac shares between every program of the user,
 * with a limit of 1 GB per cache.
 *
 * A process therefore gives the cache a home of its own below the system's
 * cache directory, beside Metal's (`<user cache directory>/<bundle
 * identifier>`), and a size limit that Mesa enforces by evicting entries.
 * Entries are keyed by the identity of the Mesa binaries that wrote them, so a
 * rebuilt framework never reads an older one's entries; they age out under the
 * limit. Call it once, at process start and before any thread can read the
 * environment: the variables are process-wide and Mesa reads them whenever a
 * screen is created. The iOS applications do the same in
 * GuestHost/Sources/MesaShaderCache.swift; the two change together.
 */

/* The cache's size limit, in MiB, and whether Mesa keeps it in its database
 * format (`MESA_DISK_CACHE_DATABASE`, a few large files) or one file per
 * entry. */
inline constexpr std::uint64_t mesa_shader_cache_limit_mib = 64;
inline constexpr bool mesa_shader_cache_uses_database = false;

struct MesaShaderCacheOutcome final {
	enum class Action {
		/* The process has no cache location: Mesa runs without a disk
		 * cache, never with one in `$HOME/.cache`. */
		unavailable,
		/* `MESA_SHADER_CACHE_DISABLE` was already set, so the caller
		 * chose and nothing changed. */
		left_to_the_caller,
		/* The variables not already set name the cache and its limit. */
		configured,
	};
	Action action {Action::unavailable};
	std::filesystem::path directory;
};

/* `<user cache directory>/<bundle identifier>`: where Mesa places its
 * `mesa_shader_cache` below when `MESA_SHADER_CACHE_DIR` names it. */
[[nodiscard]] std::filesystem::path mesa_shader_cache_directory(
	const std::filesystem::path &user_cache_directory,
	std::string_view bundle_identifier);

/* Puts the cache in `directory`, created here because Mesa creates only the
 * last component of a path. Each of `MESA_SHADER_CACHE_DIR`,
 * `MESA_SHADER_CACHE_MAX_SIZE` and `MESA_DISK_CACHE_DATABASE` that the
 * environment already has stays as it is, so a launcher or a test chooses its
 * own cache. When the directory cannot be created, `error` is set and Mesa's
 * cache is disabled instead. */
[[nodiscard]] MesaShaderCacheOutcome configure_mesa_shader_cache(
	const std::filesystem::path &directory, std::error_code &error) noexcept;

/* The same for this process's own location, from its bundle identifier
 * (`<Darwin user cache directory>/<identifier>`). A process without an
 * identifier or a cache directory has no location of its own, so its Mesa
 * cache is disabled. */
[[nodiscard]] MesaShaderCacheOutcome configure_own_mesa_shader_cache(
	std::error_code &error) noexcept;

} // namespace uml::gpu
