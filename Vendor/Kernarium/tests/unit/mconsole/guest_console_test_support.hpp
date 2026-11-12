// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "require.hpp"
#include "scratch_directory.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace uml::testing {

/* The management console's wire, as the guest's mconsole driver speaks it
 * (arch/um/drivers/mconsole.h). The host's records are private to
 * guest_console.cpp, so these are the tests' own statement of the wire: a
 * datagram socket bound where the guest would bind its console sees what a
 * guest would be asked. */
inline constexpr std::size_t maximumConsoleData = 512U;

struct ConsoleRequest final {
	std::uint32_t magic;
	std::uint32_t version;
	std::uint32_t length;
	std::array<char, maximumConsoleData> data;
};

struct ConsoleReply final {
	std::uint32_t error;
	std::uint32_t more;
	std::uint32_t length;
	std::array<char, maximumConsoleData> data;
};

/* A datagram socket bound at `path`, which it first unlinks. */
[[nodiscard]] inline int bindServer(const std::string &path)
{
	(void)::unlink(path.c_str());
	const int descriptor = ::socket(AF_UNIX, SOCK_DGRAM, 0);
	require(descriptor >= 0);
	sockaddr_un address {};
	address.sun_family = AF_UNIX;
	require(path.size() < sizeof(address.sun_path));
	std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
	require(::bind(descriptor, reinterpret_cast<const sockaddr *>(&address),
		sizeof(address)) == 0);
	return descriptor;
}

/* Waits up to `milliseconds` for a request; false when none arrives. */
[[nodiscard]] inline bool awaitRequest(int descriptor, int milliseconds)
{
	pollfd waiting {descriptor, POLLIN, 0};
	int ready;
	do {
		ready = ::poll(&waiting, 1, milliseconds);
	} while (ready < 0 && errno == EINTR);
	require(ready >= 0);
	return ready > 0;
}

/* The command of the next request, and where to send its reply. */
inline std::string receiveRequest(int descriptor, sockaddr_un &client,
	socklen_t &clientLength)
{
	ConsoleRequest request {};
	clientLength = sizeof(client);
	const auto received = ::recvfrom(descriptor, &request, sizeof(request), 0,
		reinterpret_cast<sockaddr *>(&client), &clientLength);
	require(received >= static_cast<ssize_t>(sizeof(request) -
		maximumConsoleData));
	const auto length = std::min(static_cast<std::size_t>(request.length),
		maximumConsoleData);
	return std::string(request.data.data(), length);
}

/* Answers a request with `text` and the error flag `error`; false when the
 * client has gone, as one that stopped waiting has. */
[[nodiscard]] inline bool sendReply(int descriptor, const sockaddr_un &client,
	socklen_t clientLength, std::uint32_t error, std::string_view text)
{
	require(text.size() <= maximumConsoleData);
	ConsoleReply reply {error, 0U, static_cast<std::uint32_t>(text.size()), {}};
	std::memcpy(reply.data.data(), text.data(), text.size());
	return ::sendto(descriptor, &reply, sizeof(reply), 0,
		reinterpret_cast<const sockaddr *>(&client), clientLength) ==
		static_cast<ssize_t>(sizeof(reply));
}

/* A private directory that $TMPDIR names, for a test that makes console run
 * directories. Every run removes what exited runs left in its parent,
 * $TMPDIR, so a test that plants such entries in the shared one races every
 * other run on the host, another build tree's included: one of them can
 * remove a planted directory before the test binds into it. The directory is
 * made in the user's temporary directory, not under the current $TMPDIR, so
 * that it is short enough to be the console location's first choice. It is
 * removed with its contents when the object is destroyed. */
class PrivateTemporaryDirectory final {
public:
	explicit PrivateTemporaryDirectory(std::string_view name)
		: directory_{name, userTemporaryDirectory()}
	{
		require(::setenv("TMPDIR", directory_.path().c_str(), 1) == 0);
	}

	[[nodiscard]] const std::filesystem::path &path() const noexcept
	{
		return directory_.path();
	}

private:
	[[nodiscard]] static std::filesystem::path userTemporaryDirectory()
	{
		std::array<char, 1024> base {};
		const auto length = ::confstr(_CS_DARWIN_USER_TEMP_DIR, base.data(),
			base.size());
		require(length > 0U && length <= base.size());
		return base.data();
	}

	ScratchSubdirectory directory_;
};

} // namespace uml::testing
