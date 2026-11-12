// SPDX-License-Identifier: GPL-2.0-or-later

#include "mesa_shader_cache.hpp"

#include "process_cache_location.hpp"

#include <cstdlib>
#include <string>

namespace uml::gpu {

namespace {

namespace fs = std::filesystem;

/* Sets `name` unless the environment already has it. */
void set_unless_present(const char *name, const std::string &value)
{
	::setenv(name, value.c_str(), 0);
}

void disable_mesa_shader_cache()
{
	::setenv("MESA_SHADER_CACHE_DISABLE", "true", 0);
}

} // namespace

fs::path mesa_shader_cache_directory(const fs::path &user_cache_directory,
				     std::string_view bundle_identifier)
{
	return user_cache_directory / std::string(bundle_identifier);
}

MesaShaderCacheOutcome configure_mesa_shader_cache(
	const fs::path &directory, std::error_code &error) noexcept
try {
	error.clear();
	MesaShaderCacheOutcome outcome;
	outcome.directory = directory;
	if (std::getenv("MESA_SHADER_CACHE_DISABLE") != nullptr) {
		outcome.action = MesaShaderCacheOutcome::Action::left_to_the_caller;
		return outcome;
	}
	if (std::getenv("MESA_SHADER_CACHE_DIR") == nullptr) {
		fs::create_directories(directory, error);
		if (error) {
			disable_mesa_shader_cache();
			return outcome;
		}
		set_unless_present("MESA_SHADER_CACHE_DIR", directory.string());
	}
	set_unless_present("MESA_SHADER_CACHE_MAX_SIZE",
			   std::to_string(mesa_shader_cache_limit_mib) + "M");
	if (mesa_shader_cache_uses_database)
		set_unless_present("MESA_DISK_CACHE_DATABASE", "true");
	outcome.action = MesaShaderCacheOutcome::Action::configured;
	return outcome;
} catch (...) {
	error = std::make_error_code(std::errc::not_enough_memory);
	return {};
}

MesaShaderCacheOutcome configure_own_mesa_shader_cache(
	std::error_code &error) noexcept
try {
	error.clear();
	const auto identifier = main_bundle_identifier();
	const auto root = darwin_user_cache_directory();
	if (!identifier || !root) {
		if (std::getenv("MESA_SHADER_CACHE_DISABLE") != nullptr)
			return {MesaShaderCacheOutcome::Action::left_to_the_caller, {}};
		disable_mesa_shader_cache();
		return {};
	}
	return configure_mesa_shader_cache(
		mesa_shader_cache_directory(*root, *identifier), error);
} catch (...) {
	error = std::make_error_code(std::errc::not_enough_memory);
	return {};
}

} // namespace uml::gpu
