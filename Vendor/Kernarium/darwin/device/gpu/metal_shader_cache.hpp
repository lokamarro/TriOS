// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

namespace uml::gpu {

/*
 * Metal keeps one shader cache per process identity, and the first pipeline a
 * process compiles reads the whole index of that cache into memory. The cache
 * grows with every distinct shader a process compiles, and Metal bounds only
 * each data file, near 1.1 GB, not the index. A host that has run GPU
 * conformance campaigns therefore pays tens of megabytes of footprint and a
 * fraction of a second at the first compile of every later process.
 *
 * Metal has no call that clears or bounds the cache, and nothing in it says
 * which entries came from a guest shader. A process therefore bounds its own
 * cache by measuring the index before anything in the process can open it, and
 * removes the cache once the index is too large. The cache is only an
 * optimization: the next compile fills it again. A process without a bundle
 * identifier shares one cache with every other such process of the user, and
 * has no cache of its own to bound.
 */

/* The index bytes a cache may hold and still be kept. Real guest workloads
 * leave about 0.12 MB of index and repeating them adds none, while an index
 * that Metal has let grow to its own ceiling, about 23 MB, costs around
 * 100 MiB. The same limit bounds Kernarium's cache
 * (GuestHost/Sources/MetalShaderCache.swift). */
inline constexpr std::uint64_t metal_shader_cache_index_limit = 1'000'000;

struct MetalShaderCacheSize final {
	/* Bytes in the `.list` files, which the first compile reads. */
	std::uint64_t index {};
	/* Bytes in every other file, which the process maps and pages in only
	 * as entries are used. */
	std::uint64_t data {};
};

struct MetalShaderCacheOutcome final {
	enum class Action { absent, kept, removed };
	Action action {Action::absent};
	MetalShaderCacheSize size;
	std::filesystem::path directory;
};

/* `<user cache directory>/<bundle identifier>/com.apple.metal`: where Metal
 * keeps the cache of a process with that identifier. The layout is Metal's
 * own and undocumented; if it changes, no cache is found and none removed. */
[[nodiscard]] std::filesystem::path metal_shader_cache_directory(
	const std::filesystem::path &user_cache_directory,
	std::string_view bundle_identifier);

/* The size of the cache at `directory`, or nothing when there is none. The
 * index files sit at different depths (below a GPU-named directory), so every
 * `.list` file below the directory counts. A file that disappears during the
 * walk, because another process removed the cache, is not counted. */
[[nodiscard]] std::optional<MetalShaderCacheSize> metal_shader_cache_size(
	const std::filesystem::path &directory) noexcept;

/* Removes the cache at `directory` when its index holds more than
 * `index_limit` bytes; a cache exactly at the limit is kept. Other processes
 * may be using the cache, or bounding it at the same moment: a process keeps
 * the files it has open, one that finds the directory gone creates it again,
 * and a removal that another process got to first is not an error. `error` is
 * set when the removal fails otherwise, and the outcome then reports the cache
 * as kept. */
[[nodiscard]] MetalShaderCacheOutcome bound_metal_shader_cache(
	const std::filesystem::path &directory, std::uint64_t index_limit,
	std::error_code &error) noexcept;

/* Applies `metal_shader_cache_index_limit` to this process's own cache. Call
 * it once, before anything creates a Metal device: once Metal has read the
 * index, removing the files no longer returns its memory. Nothing happens, and
 * nothing is returned, in a process without a bundle identifier or when the
 * Darwin user cache directory is unknown. */
[[nodiscard]] std::optional<MetalShaderCacheOutcome>
bound_own_metal_shader_cache(std::error_code &error) noexcept;

} // namespace uml::gpu
