// SPDX-License-Identifier: GPL-2.0-or-later

#include "mesa_shader_cache.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace {

using uml::gpu::MesaShaderCacheOutcome;
using uml::testing::require;
using uml::testing::ScratchSubdirectory;
namespace fs = std::filesystem;

constexpr const char *variables[] = {
	"MESA_SHADER_CACHE_DISABLE",
	"MESA_SHADER_CACHE_DIR",
	"MESA_SHADER_CACHE_MAX_SIZE",
	"MESA_DISK_CACHE_DATABASE",
};

/* The process environment as a test found it, put back when the test ends,
 * and cleared of the variables Mesa reads while it runs. */
class CleanEnvironment final {
public:
	CleanEnvironment()
	{
		for (std::size_t index = 0; index < std::size(variables); ++index) {
			if (const char *value = std::getenv(variables[index]))
				saved_[index] = value;
			::unsetenv(variables[index]);
		}
	}
	CleanEnvironment(const CleanEnvironment &) = delete;
	CleanEnvironment &operator=(const CleanEnvironment &) = delete;
	~CleanEnvironment()
	{
		for (std::size_t index = 0; index < std::size(variables); ++index) {
			if (saved_[index])
				::setenv(variables[index], saved_[index]->c_str(), 1);
			else
				::unsetenv(variables[index]);
		}
	}

private:
	std::optional<std::string> saved_[std::size(variables)];
};

[[nodiscard]] std::optional<std::string> value(const char *name)
{
	if (const char *text = std::getenv(name))
		return std::string(text);
	return std::nullopt;
}

void theDirectoryIsNamedAfterTheBundleIdentifier()
{
	require(uml::gpu::mesa_shader_cache_directory(
			"/cache/root/", "com.example.app") ==
		fs::path("/cache/root/com.example.app"));
}

/* Mesa creates only the last component of the path it is given. */
void theCacheIsPlacedAndBounded()
{
	const CleanEnvironment environment;
	const ScratchSubdirectory root{"uml-mesa-cache-XXXXXX"};
	const auto directory = uml::gpu::mesa_shader_cache_directory(
		root.path() / "caches", "com.example.app");
	std::error_code error;
	const auto outcome =
		uml::gpu::configure_mesa_shader_cache(directory, error);
	require(!error);
	require(outcome.action == MesaShaderCacheOutcome::Action::configured);
	require(outcome.directory == directory);
	require(fs::is_directory(directory));
	require(value("MESA_SHADER_CACHE_DIR") == directory.string());
	require(value("MESA_SHADER_CACHE_MAX_SIZE") ==
		std::to_string(uml::gpu::mesa_shader_cache_limit_mib) + "M");
	require(value("MESA_DISK_CACHE_DATABASE").has_value() ==
		uml::gpu::mesa_shader_cache_uses_database);
	require(!value("MESA_SHADER_CACHE_DISABLE"));
}

/* A launcher or a test that names its own cache keeps it. */
void aVariableTheEnvironmentHasStaysAsItIs()
{
	const CleanEnvironment environment;
	const ScratchSubdirectory root{"uml-mesa-cache-XXXXXX"};
	::setenv("MESA_SHADER_CACHE_DIR", "/named/by/the/caller", 1);
	::setenv("MESA_SHADER_CACHE_MAX_SIZE", "17M", 1);
	const auto directory = root.path() / "unused";
	std::error_code error;
	const auto outcome =
		uml::gpu::configure_mesa_shader_cache(directory, error);
	require(!error);
	require(outcome.action == MesaShaderCacheOutcome::Action::configured);
	require(value("MESA_SHADER_CACHE_DIR") == "/named/by/the/caller");
	require(value("MESA_SHADER_CACHE_MAX_SIZE") == "17M");
	require(!fs::exists(directory));
}

void aCacheTheCallerDisabledStaysDisabled()
{
	const CleanEnvironment environment;
	const ScratchSubdirectory root{"uml-mesa-cache-XXXXXX"};
	::setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
	const auto directory = root.path() / "caches";
	std::error_code error;
	const auto outcome =
		uml::gpu::configure_mesa_shader_cache(directory, error);
	require(!error);
	require(outcome.action ==
		MesaShaderCacheOutcome::Action::left_to_the_caller);
	require(value("MESA_SHADER_CACHE_DISABLE") == "true");
	require(!value("MESA_SHADER_CACHE_DIR"));
	require(!value("MESA_SHADER_CACHE_MAX_SIZE"));
	require(!fs::exists(directory));
}

/* The cache must not fall back to $HOME/.cache, where a device container
 * does not let Mesa create it and a Mac shares it between programs. */
void aDirectoryThatCannotBeCreatedDisablesTheCache()
{
	const CleanEnvironment environment;
	const ScratchSubdirectory root{"uml-mesa-cache-XXXXXX"};
	const auto file = root.path() / "file";
	std::ofstream(file) << "x";
	std::error_code error;
	const auto outcome = uml::gpu::configure_mesa_shader_cache(
		file / "caches", error);
	require(static_cast<bool>(error));
	require(outcome.action == MesaShaderCacheOutcome::Action::unavailable);
	require(value("MESA_SHADER_CACHE_DISABLE") == "true");
	require(!value("MESA_SHADER_CACHE_DIR"));
}

/* The test binary has no Info.plist, as every process without a bundle
 * identifier: it has no location of its own, so Mesa gets no cache. */
void aProcessWithoutAnIdentifierGetsNoCache()
{
	const CleanEnvironment environment;
	std::error_code error;
	const auto outcome = uml::gpu::configure_own_mesa_shader_cache(error);
	require(!error);
	require(outcome.action == MesaShaderCacheOutcome::Action::unavailable);
	require(value("MESA_SHADER_CACHE_DISABLE") == "true");
	require(!value("MESA_SHADER_CACHE_DIR"));
}

} // namespace

int main()
{
	theDirectoryIsNamedAfterTheBundleIdentifier();
	theCacheIsPlacedAndBounded();
	aVariableTheEnvironmentHasStaysAsItIs();
	aCacheTheCallerDisabledStaysDisabled();
	aDirectoryThatCannotBeCreatedDisablesTheCache();
	aProcessWithoutAnIdentifierGetsNoCache();
	return 0;
}
