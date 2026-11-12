// SPDX-License-Identifier: GPL-2.0-or-later

#include "require.hpp"
#include "scratch_directory.hpp"
#include "socket_paths.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

using uml::testing::require;
using uml::testing::requireThrows;

/* Socket paths must fit sun_path, so they live in a fresh directory under the
 * host temporary directory rather than the build-tree scratch directory. */
const std::filesystem::path &scratchDirectory()
{
	static const uml::testing::ScratchSubdirectory directory {
		"uml-socket-path.XXXXXX", uml::testing::socket_scratch_parent()};
	return directory.path();
}

bool socketExists(const std::filesystem::path &path)
{
	struct stat status {};
	return ::lstat(path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode);
}

/* Binds a listening Unix socket at `path` and returns its descriptor. */
int bindSocket(const std::filesystem::path &path)
{
	const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
	require(descriptor >= 0);
	sockaddr_un address {};
	address.sun_family = AF_UNIX;
	const auto text = path.string();
	require(text.size() < sizeof(address.sun_path));
	std::memcpy(address.sun_path, text.c_str(), text.size() + 1U);
	require(::bind(descriptor,
		       reinterpret_cast<const sockaddr *>(&address),
		       sizeof(address)) == 0);
	return descriptor;
}

void anAdoptedPathIsRemovedWhenTheGuardIsDestroyed()
{
	const auto path = scratchDirectory() / "destroyed.sock";
	std::filesystem::remove(path);
	const int descriptor = bindSocket(path);
	{
		::uml::device::SocketPath guard {path};
		guard.adopt();
		require(socketExists(path));
	}
	require(!socketExists(path));
	(void)::close(descriptor);
}

void anUnadoptedPathIsLeftAlone()
{
	const auto path = scratchDirectory() / "unadopted.sock";
	std::filesystem::remove(path);
	const int descriptor = bindSocket(path);
	{
		/* bind() failed in the caller, so the guard never took
		 * ownership and must not remove a path it does not own. */
		::uml::device::SocketPath guard {path};
	}
	require(socketExists(path));
	(void)::close(descriptor);
	std::filesystem::remove(path);
}

void theSignalPathRemovesEveryAdoptedSocket()
{
	const auto first = scratchDirectory() / "signal-first.sock";
	const auto second = scratchDirectory() / "signal-second.sock";
	std::filesystem::remove(first);
	std::filesystem::remove(second);
	const int firstDescriptor = bindSocket(first);
	const int secondDescriptor = bindSocket(second);
	::uml::device::SocketPath firstGuard {first};
	::uml::device::SocketPath secondGuard {second};
	firstGuard.adopt();
	secondGuard.adopt();
	require(socketExists(first) && socketExists(second));

	/* What the terminating-signal handler runs.  Neither guard unwinds in
	 * that path, so this call alone has to release both. */
	::uml::device::remove_registered_socket_paths();
	require(!socketExists(first) && !socketExists(second));
	(void)::close(firstDescriptor);
	(void)::close(secondDescriptor);
}

void aReplacedPathSurvives()
{
	const auto path = scratchDirectory() / "replaced.sock";
	std::filesystem::remove(path);
	const int original = bindSocket(path);
	::uml::device::SocketPath guard {path};
	guard.adopt();

	/* Another process removed our socket and bound its own at the same
	 * name.  The recorded device and inode no longer match, so neither the
	 * signal path nor the destructor may remove the replacement. */
	std::filesystem::remove(path);
	const int replacement = bindSocket(path);
	::uml::device::remove_registered_socket_paths();
	require(socketExists(path));
	(void)::close(original);
	(void)::close(replacement);
	std::filesystem::remove(path);
}

void aPathTooLongForSunPathIsRefused()
{
	const auto path = scratchDirectory() /
		std::string(sizeof(sockaddr_un::sun_path), 'n');
	::uml::device::SocketPath guard {path};
	requireThrows([&] { guard.adopt(); });
}

void anUnboundPathIsRefused()
{
	const auto path = scratchDirectory() / "never-bound.sock";
	std::filesystem::remove(path);
	::uml::device::SocketPath guard {path};
	requireThrows([&] { guard.adopt(); });
}

void theTableRefusesMoreThanItCanHold()
{
	std::vector<int> descriptors;
	std::vector<std::unique_ptr<::uml::device::SocketPath>> guards;
	bool refused = false;
	/* One more than the table holds, so the last adopt() must report the
	 * exhaustion instead of silently dropping a path. */
	for (int index = 0; index < 17; ++index) {
		const auto path = scratchDirectory() /
			("slot-" + std::to_string(index) + ".sock");
		std::filesystem::remove(path);
		descriptors.push_back(bindSocket(path));
		auto guard = std::make_unique<::uml::device::SocketPath>(path);
		try {
			guard->adopt();
		} catch (const std::exception &) {
			refused = true;
			break;
		}
		guards.push_back(std::move(guard));
	}
	require(refused);
	guards.clear();
	for (const int descriptor : descriptors)
		(void)::close(descriptor);
	for (int index = 0; index < 17; ++index)
		std::filesystem::remove(scratchDirectory() /
			("slot-" + std::to_string(index) + ".sock"));
}

bool directoryExists(const std::filesystem::path &path)
{
	struct stat status {};
	return ::lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode);
}

/* A fresh directory for one DirectoryPath case. */
std::filesystem::path makeDirectory(const char *name)
{
	const auto path = scratchDirectory() / name;
	std::filesystem::remove_all(path);
	require(::mkdir(path.c_str(), S_IRWXU) == 0);
	return path;
}

void anAdoptedDirectoryIsRemovedWithEverythingInIt()
{
	const auto path = makeDirectory("destroyed.dir");
	const int descriptor = bindSocket(path / "entry");
	{
		::uml::device::DirectoryPath guard {path};
		guard.adopt("entry");
		/* An ordinary teardown also takes what the entry's name does
		 * not cover. */
		std::filesystem::create_directory(path / "nested");
		require(directoryExists(path));
	}
	require(!std::filesystem::exists(path));
	(void)::close(descriptor);
}

void anUnadoptedDirectoryIsLeftAlone()
{
	const auto path = makeDirectory("unadopted.dir");
	{
		::uml::device::DirectoryPath guard {path};
	}
	require(directoryExists(path));
	std::filesystem::remove_all(path);
}

void theSignalPathRemovesAnAdoptedDirectory()
{
	const auto path = makeDirectory("signal.dir");
	::uml::device::DirectoryPath guard {path};
	guard.adopt("entry");
	/* The entry is bound after adoption by someone else, as the guest
	 * binds its console, and a reply socket beside it is adopted. */
	const int entry = bindSocket(path / "entry");
	const int reply = bindSocket(path / "reply");
	::uml::device::SocketPath replyGuard {path / "reply"};
	replyGuard.adopt();

	/* What a terminating signal or a kernel-driven exit runs; no guard
	 * unwinds there. Sockets go first, so the directory is empty when its
	 * turn comes. */
	::uml::device::remove_registered_socket_paths();
	require(!std::filesystem::exists(path));
	(void)::close(entry);
	(void)::close(reply);
}

void aDirectoryWithAnUnknownEntryKeepsIt()
{
	const auto path = makeDirectory("unknown.dir");
	::uml::device::DirectoryPath guard {path};
	guard.adopt("entry");
	const int entry = bindSocket(path / "entry");
	const int stranger = bindSocket(path / "stranger");

	/* The termination path removes only the name it knows, and rmdir()
	 * then refuses a directory that is not empty. */
	::uml::device::remove_registered_socket_paths();
	require(directoryExists(path) && !socketExists(path / "entry") &&
		socketExists(path / "stranger"));
	(void)::close(entry);
	(void)::close(stranger);
	std::filesystem::remove_all(path);
}

void aReplacedDirectorySurvives()
{
	const auto path = makeDirectory("replaced.dir");
	::uml::device::DirectoryPath guard {path};
	guard.adopt("entry");

	/* Something else now owns the name; its device and inode differ. */
	std::filesystem::remove(path);
	require(::mkdir(path.c_str(), S_IRWXU) == 0);
	const int entry = bindSocket(path / "entry");
	::uml::device::remove_registered_socket_paths();
	require(directoryExists(path) && socketExists(path / "entry"));
	(void)::close(entry);
	std::filesystem::remove_all(path);
}

void aMissingDirectoryIsRefused()
{
	const auto path = scratchDirectory() / "never-created.dir";
	std::filesystem::remove_all(path);
	::uml::device::DirectoryPath guard {path};
	requireThrows([&] { guard.adopt("entry"); });
}

} // namespace

int main()
{
	anAdoptedPathIsRemovedWhenTheGuardIsDestroyed();
	anUnadoptedPathIsLeftAlone();
	theSignalPathRemovesEveryAdoptedSocket();
	aReplacedPathSurvives();
	aPathTooLongForSunPathIsRefused();
	anUnboundPathIsRefused();
	theTableRefusesMoreThanItCanHold();
	anAdoptedDirectoryIsRemovedWithEverythingInIt();
	anUnadoptedDirectoryIsLeftAlone();
	theSignalPathRemovesAnAdoptedDirectory();
	aDirectoryWithAnUnknownEntryKeepsIt();
	aReplacedDirectorySurvives();
	aMissingDirectoryIsRefused();
	return EXIT_SUCCESS;
}
