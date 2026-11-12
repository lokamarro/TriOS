// SPDX-License-Identifier: GPL-2.0-or-later
#include "scratch_directory.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace uml::testing {
namespace {

[[noreturn]] void fail(const char *message, const std::filesystem::path &path,
	const std::error_code &error)
{
	std::fprintf(stderr, "test scratch directory: %s %s: %s\n", message,
		path.c_str(), error.message().c_str());
	std::abort();
}

[[nodiscard]] std::filesystem::path host_temporary_directory()
{
	std::error_code error;
	auto directory = std::filesystem::temp_directory_path(error);
	if (error)
		fail("cannot resolve the host temporary directory", "$TMPDIR",
			error);
	return directory;
}

[[nodiscard]] std::string make_template(
	const std::filesystem::path &directory, std::string_view name)
{
	if (!name.ends_with("XXXXXX") ||
	    name.find('/') != std::string_view::npos)
		fail("rejects template name", std::filesystem::path{name},
			std::make_error_code(std::errc::invalid_argument));
	return (directory / name).string();
}

} // namespace

const std::filesystem::path &scratch_directory()
{
	static const std::filesystem::path directory = [] {
		const auto *configured =
			std::getenv("UML_TEST_SCRATCH_DIRECTORY");
		std::filesystem::path result =
			configured != nullptr && configured[0] != '\0'
				? std::filesystem::path{configured}
				: host_temporary_directory();
		std::error_code error;
		std::filesystem::create_directories(result, error);
		if (error)
			fail("cannot create", result, error);
		return result;
	}();
	return directory;
}

std::string scratch_template(std::string_view name)
{
	return make_template(scratch_directory(), name);
}

std::string socket_scratch_template(std::string_view name)
{
	return make_template(socket_scratch_parent(), name);
}

std::filesystem::path socket_scratch_parent()
{
	return host_temporary_directory();
}

ScratchSubdirectory::ScratchSubdirectory(std::string_view name,
	const std::filesystem::path &parent)
{
	auto pattern = make_template(parent, name);
	if (::mkdtemp(pattern.data()) == nullptr)
		fail("cannot create", pattern,
			std::error_code{errno, std::generic_category()});
	path_ = std::move(pattern);
}

ScratchSubdirectory::~ScratchSubdirectory()
{
	std::error_code ignored;
	std::filesystem::remove_all(path_, ignored);
}

} // namespace uml::testing
