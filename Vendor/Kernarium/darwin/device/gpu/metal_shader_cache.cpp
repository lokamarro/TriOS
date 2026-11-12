// SPDX-License-Identifier: GPL-2.0-or-later

#include "metal_shader_cache.hpp"

#include "process_cache_location.hpp"

#include <string>

namespace uml::gpu {

namespace {

namespace fs = std::filesystem;

} // namespace

fs::path metal_shader_cache_directory(const fs::path &user_cache_directory,
				      std::string_view bundle_identifier)
{
	return user_cache_directory / std::string(bundle_identifier) /
		"com.apple.metal";
}

std::optional<MetalShaderCacheSize> metal_shader_cache_size(
	const fs::path &directory) noexcept
try {
	std::error_code error;
	if (!fs::exists(directory, error))
		return std::nullopt;
	MetalShaderCacheSize size;
	fs::recursive_directory_iterator files(
		directory, fs::directory_options::skip_permission_denied, error);
	for (; !error && files != fs::recursive_directory_iterator {};
	     files.increment(error)) {
		std::error_code entry_error;
		if (files->symlink_status(entry_error).type() !=
			    fs::file_type::regular ||
		    entry_error)
			continue;
		const std::uintmax_t bytes = files->file_size(entry_error);
		if (entry_error)
			continue;
		if (files->path().extension() == ".list")
			size.index += bytes;
		else
			size.data += bytes;
	}
	return size;
} catch (...) {
	return std::nullopt;
}

MetalShaderCacheOutcome bound_metal_shader_cache(
	const fs::path &directory, std::uint64_t index_limit,
	std::error_code &error) noexcept
try {
	error.clear();
	MetalShaderCacheOutcome outcome;
	outcome.directory = directory;
	const auto size = metal_shader_cache_size(directory);
	if (!size)
		return outcome;
	outcome.size = *size;
	outcome.action = MetalShaderCacheOutcome::Action::kept;
	if (size->index <= index_limit)
		return outcome;
	fs::remove_all(directory, error);
	if (!error) {
		outcome.action = MetalShaderCacheOutcome::Action::removed;
	} else if (error == std::errc::no_such_file_or_directory) {
		/* Another process bounded the same cache first. */
		error.clear();
		outcome.action = MetalShaderCacheOutcome::Action::absent;
	} else if (error == std::errc::directory_not_empty) {
		/* Another process began a cache here while this one removed the
		 * old files, which are gone. */
		error.clear();
		outcome.action = MetalShaderCacheOutcome::Action::removed;
	}
	return outcome;
} catch (...) {
	error = std::make_error_code(std::errc::not_enough_memory);
	return {};
}

std::optional<MetalShaderCacheOutcome>
bound_own_metal_shader_cache(std::error_code &error) noexcept
try {
	error.clear();
	const auto identifier = main_bundle_identifier();
	const auto root = darwin_user_cache_directory();
	if (!identifier || !root)
		return std::nullopt;
	return bound_metal_shader_cache(
		metal_shader_cache_directory(*root, *identifier),
		metal_shader_cache_index_limit, error);
} catch (...) {
	error = std::make_error_code(std::errc::not_enough_memory);
	return std::nullopt;
}

} // namespace uml::gpu
