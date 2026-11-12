// SPDX-License-Identifier: GPL-2.0-or-later

#include "metal_shader_cache.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <fstream>
#include <string>
#include <system_error>

namespace {

using uml::gpu::MetalShaderCacheOutcome;
using uml::testing::require;
using uml::testing::ScratchSubdirectory;
namespace fs = std::filesystem;

void write(const fs::path &file, std::size_t bytes)
{
	fs::create_directories(file.parent_path());
	std::ofstream stream(file, std::ios::binary);
	stream << std::string(bytes, 'x');
	require(stream.good());
}

/* The layout Metal uses: the GPU binaries' pair below a GPU-named directory,
 * the front end's below another, and an empty timestamp beside them. */
void populate(const fs::path &cache, std::size_t functions_index,
	      std::size_t libraries_index)
{
	write(cache / "16777235_594" / "functions.list", functions_index);
	write(cache / "16777235_594" / "functions.data", 8000);
	write(cache / "32024" / "libraries.list", libraries_index);
	write(cache / "32024" / "libraries.data", 5024);
}

void theDirectoryIsNamedAfterTheBundleIdentifier()
{
	require(uml::gpu::metal_shader_cache_directory(
			"/cache/root/", "com.example.app") ==
		fs::path("/cache/root/com.example.app/com.apple.metal"));
}

void anAbsentCacheIsReportedAbsent()
{
	const ScratchSubdirectory root{"uml-metal-cache-XXXXXX"};
	const auto cache = uml::gpu::metal_shader_cache_directory(
		root.path(), "com.example.app");
	require(!uml::gpu::metal_shader_cache_size(cache));
	std::error_code error;
	const auto outcome =
		uml::gpu::bound_metal_shader_cache(cache, 1000, error);
	require(!error);
	require(outcome.action == MetalShaderCacheOutcome::Action::absent);
}

void indexAndDataAreCountedSeparately()
{
	const ScratchSubdirectory root{"uml-metal-cache-XXXXXX"};
	const auto cache = uml::gpu::metal_shader_cache_directory(
		root.path(), "com.example.app");
	populate(cache, 600, 400);
	const auto size = uml::gpu::metal_shader_cache_size(cache);
	require(size.has_value());
	require(size->index == 1000U);
	require(size->data == 13024U);
}

void aCacheAtTheLimitIsKept()
{
	const ScratchSubdirectory root{"uml-metal-cache-XXXXXX"};
	const auto cache = uml::gpu::metal_shader_cache_directory(
		root.path(), "com.example.app");
	populate(cache, 600, 400);
	std::error_code error;
	const auto outcome =
		uml::gpu::bound_metal_shader_cache(cache, 1000, error);
	require(!error);
	require(outcome.action == MetalShaderCacheOutcome::Action::kept);
	require(outcome.size.index == 1000U);
	require(fs::exists(cache / "32024" / "libraries.list"));
}

void aCacheOverTheLimitIsRemovedWhole()
{
	const ScratchSubdirectory root{"uml-metal-cache-XXXXXX"};
	const auto cache = uml::gpu::metal_shader_cache_directory(
		root.path(), "com.example.app");
	populate(cache, 601, 400);
	std::error_code error;
	const auto outcome =
		uml::gpu::bound_metal_shader_cache(cache, 1000, error);
	require(!error);
	require(outcome.action == MetalShaderCacheOutcome::Action::removed);
	require(outcome.size.index == 1001U);
	require(outcome.size.data == 13024U);
	require(outcome.directory == cache);
	require(!fs::exists(cache));
}

/* Another identifier's cache and the identifier's own parent are not the
 * cache, so a bounded cache takes nothing else with it. */
void onlyTheCacheOfThatIdentifierIsRemoved()
{
	const ScratchSubdirectory root{"uml-metal-cache-XXXXXX"};
	const auto own = uml::gpu::metal_shader_cache_directory(
		root.path(), "com.example.app");
	const auto other = uml::gpu::metal_shader_cache_directory(
		root.path(), "com.example.other");
	populate(own, 2000, 2000);
	populate(other, 2000, 2000);
	write(own.parent_path() / "preferences.txt", 10);
	std::error_code error;
	const auto outcome =
		uml::gpu::bound_metal_shader_cache(own, 1000, error);
	require(!error);
	require(outcome.action == MetalShaderCacheOutcome::Action::removed);
	require(!fs::exists(own));
	require(fs::exists(own.parent_path() / "preferences.txt"));
	require(fs::exists(other / "32024" / "libraries.list"));
}

/* A symlink in the cache is not followed out of it, neither when counting
 * nor when removing. */
void aSymlinkIsNeitherCountedNorFollowed()
{
	const ScratchSubdirectory root{"uml-metal-cache-XXXXXX"};
	const auto cache = uml::gpu::metal_shader_cache_directory(
		root.path(), "com.example.app");
	const auto outside = root.path() / "outside";
	write(outside / "big.list", 5000);
	populate(cache, 600, 400);
	fs::create_directory_symlink(outside, cache / "link");
	fs::create_symlink(outside / "big.list", cache / "link.list");
	const auto size = uml::gpu::metal_shader_cache_size(cache);
	require(size.has_value());
	require(size->index == 1000U);
	std::error_code error;
	const auto outcome =
		uml::gpu::bound_metal_shader_cache(cache, 999, error);
	require(!error);
	require(outcome.action == MetalShaderCacheOutcome::Action::removed);
	require(!fs::exists(cache));
	require(fs::exists(outside / "big.list"));
}

/* The test binary has no Info.plist, as every process without a bundle
 * identifier: it has no cache of its own to bound. */
void aProcessWithoutAnIdentifierBoundsNothing()
{
	std::error_code error;
	require(!uml::gpu::bound_own_metal_shader_cache(error));
	require(!error);
}

} // namespace

int main()
{
	theDirectoryIsNamedAfterTheBundleIdentifier();
	anAbsentCacheIsReportedAbsent();
	indexAndDataAreCountedSeparately();
	aCacheAtTheLimitIsKept();
	aCacheOverTheLimitIsRemovedWhole();
	onlyTheCacheOfThatIdentifierIsRemoved();
	aSymlinkIsNeitherCountedNorFollowed();
	aProcessWithoutAnIdentifierBoundsNothing();
	return 0;
}
