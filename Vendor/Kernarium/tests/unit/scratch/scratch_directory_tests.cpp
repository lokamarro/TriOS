// SPDX-License-Identifier: GPL-2.0-or-later
#include "require.hpp"
#include "scratch_directory.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using uml::testing::require;

/* The caller has not resolved its own scratch directory yet, so the child
 * resolves it afresh from the environment it sets up. */
template<typename Body>
void requireInChild(Body &&body, const char *message)
{
	const pid_t child = ::fork();
	require(child >= 0, "cannot fork");
	if (child == 0)
		::_exit(body() ? 0 : 1);
	int status{};
	require(::waitpid(child, &status, 0) == child, "cannot wait for child");
	require(WIFEXITED(status) && WEXITSTATUS(status) == 0, message);
}

} // namespace

int main()
{
	const auto *configured = std::getenv("UML_TEST_SCRATCH_DIRECTORY");
	require(configured != nullptr && configured[0] != '\0',
		"the unit registration names no scratch directory");
	const std::filesystem::path perTest{configured};
	require(perTest.is_absolute(), "the scratch directory is relative");
	const auto *test = std::getenv("UML_UNIT_TEST_NAME");
	require(test != nullptr && perTest.filename() == test,
		"the scratch directory is not private to this test");

	const auto hostTemporary = perTest / "host-temporary";
	std::filesystem::create_directories(hostTemporary);
	requireInChild([&] {
		if (::unsetenv("UML_TEST_SCRATCH_DIRECTORY") != 0 ||
		    ::setenv("TMPDIR", hostTemporary.c_str(), 1) != 0)
			return false;
		return uml::testing::scratch_directory() == hostTemporary;
	}, "a binary run outside CTest does not use $TMPDIR");

	const auto nested = perTest / "created" / "on-first-use";
	requireInChild([&] {
		const auto *value = nested.c_str();
		if (::setenv("UML_TEST_SCRATCH_DIRECTORY", value, 1) != 0)
			return false;
		return uml::testing::scratch_directory() == nested &&
			std::filesystem::is_directory(nested);
	}, "the scratch directory is not created on first use");

	require(uml::testing::scratch_directory() == perTest,
		"the scratch directory differs from the registration");
	auto path = uml::testing::scratch_template("uml-scratch-test.XXXXXX");
	const int descriptor = ::mkstemp(path.data());
	require(descriptor >= 0, "cannot create a scratch file");
	require(std::filesystem::path{path}.parent_path() == perTest,
		"a scratch file left the scratch directory");
	require(::close(descriptor) == 0 && ::unlink(path.c_str()) == 0,
		"cannot remove a scratch file");

	const std::filesystem::path socket =
		uml::testing::socket_scratch_template("uml-socket.XXXXXX");
	require(std::filesystem::equivalent(socket.parent_path(),
			std::filesystem::temp_directory_path()),
		"a socket path left the host temporary directory");

	std::filesystem::path subdirectory;
	{
		const uml::testing::ScratchSubdirectory scratch{
			"uml-subdirectory.XXXXXX"};
		subdirectory = scratch.path();
		require(subdirectory.parent_path() == perTest &&
			std::filesystem::is_directory(subdirectory),
			"a scratch subdirectory is not made inside the scratch directory");
		std::filesystem::create_directories(subdirectory / "inner");
		std::ofstream{subdirectory / "inner" / "file"} << "contents";
	}
	require(!std::filesystem::exists(subdirectory),
		"a scratch subdirectory outlived its object");
	{
		const uml::testing::ScratchSubdirectory scratch{
			"uml-socket-subdirectory.XXXXXX",
			uml::testing::socket_scratch_parent()};
		require(std::filesystem::equivalent(scratch.path().parent_path(),
				std::filesystem::temp_directory_path()),
			"a socket subdirectory left the host temporary directory");
	}

	std::filesystem::remove_all(hostTemporary);
	std::filesystem::remove_all(perTest / "created");
	return 0;
}
