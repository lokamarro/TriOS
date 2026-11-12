// SPDX-License-Identifier: GPL-2.0-or-later
#include "socket_paths.hpp"

#include <array>
#include <atomic>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace uml::device {
namespace {

/*
 * One process binds a console, a serial line, and one socket per vhost-user
 * device, so a small fixed table covers every supported composition.  The
 * storage is constant-initialized rather than lazily constructed: a function
 * scope static would place a guard variable on the path a signal handler
 * takes.
 */
constexpr std::size_t maximum_socket_paths = 16;

struct Slot final {
	std::array<char, sizeof(sockaddr_un::sun_path)> path{};
	dev_t device{};
	ino_t inode{};
	/* Published last with release ordering, so a handler that observes it
	 * also observes a complete path and identity. */
	std::atomic_bool published{};
	std::atomic_bool claimed{};
};

std::array<Slot, maximum_socket_paths> table{};

/* The guest console's directory and the launcher's initrd scratch. */
constexpr std::size_t maximum_directories = 4;

struct DirectorySlot final {
	std::array<char, PATH_MAX> path{};
	std::array<char, PATH_MAX> entry{};
	dev_t device{};
	ino_t inode{};
	std::atomic_bool published{};
	std::atomic_bool claimed{};
};

std::array<DirectorySlot, maximum_directories> directories{};

void remove_slot(const Slot &slot) noexcept
{
	struct stat status {};
	if (::lstat(slot.path.data(), &status) != 0 ||
	    !S_ISSOCK(status.st_mode) || status.st_dev != slot.device ||
	    status.st_ino != slot.inode)
		return;
	(void)::unlink(slot.path.data());
}

[[nodiscard]] bool is_adopted_directory(const DirectorySlot &slot) noexcept
{
	struct stat status {};
	return ::lstat(slot.path.data(), &status) == 0 &&
		S_ISDIR(status.st_mode) && status.st_dev == slot.device &&
		status.st_ino == slot.inode;
}

/* Only the directory's identity is checked: it belongs to this process alone,
 * so whatever has the entry's name inside it is this run's. */
void remove_directory_slot(const DirectorySlot &slot) noexcept
{
	if (!is_adopted_directory(slot))
		return;
	struct stat status {};
	if (::lstat(slot.entry.data(), &status) == 0 &&
	    !S_ISDIR(status.st_mode))
		(void)::unlink(slot.entry.data());
	(void)::rmdir(slot.path.data());
}

void handle_termination(int signal) noexcept
{
	remove_registered_socket_paths();
	struct sigaction action {};
	action.sa_handler = SIG_DFL;
	sigemptyset(&action.sa_mask);
	(void)::sigaction(signal, &action, nullptr);
	(void)::raise(signal);
}

} // namespace

SocketPath::SocketPath(std::filesystem::path path)
	: path_(std::move(path))
{
}

SocketPath::~SocketPath()
{
	if (slot_ < 0)
		return;
	auto &slot = table[static_cast<std::size_t>(slot_)];
	/* Unlink before retiring the slot.  A signal delivered in between then
	 * finds the path already gone rather than a slot that no longer names
	 * work the handler would have completed. */
	remove_slot(slot);
	slot.published.store(false, std::memory_order_release);
	slot.claimed.store(false, std::memory_order_release);
}

void SocketPath::adopt()
{
	const auto text = path_.string();
	if (text.size() >= sizeof(sockaddr_un::sun_path))
		throw std::runtime_error("socket path is too long: " + text);
	struct stat status {};
	if (::lstat(path_.c_str(), &status) != 0 || !S_ISSOCK(status.st_mode))
		throw std::runtime_error(
			"bound socket path changed unexpectedly: " + text);
	for (std::size_t index = 0; index < maximum_socket_paths; ++index) {
		auto &slot = table[index];
		bool unclaimed = false;
		if (!slot.claimed.compare_exchange_strong(unclaimed, true,
			    std::memory_order_acq_rel))
			continue;
		std::memcpy(slot.path.data(), text.c_str(), text.size() + 1U);
		slot.device = status.st_dev;
		slot.inode = status.st_ino;
		slot.published.store(true, std::memory_order_release);
		slot_ = static_cast<int>(index);
		return;
	}
	throw std::runtime_error(
		"no free socket-path cleanup slot for " + text);
}

DirectoryPath::DirectoryPath(std::filesystem::path path)
	: path_(std::move(path))
{
}

DirectoryPath::~DirectoryPath()
{
	if (slot_ < 0)
		return;
	auto &slot = directories[static_cast<std::size_t>(slot_)];
	/* As for a socket path: remove first, then retire the slot. */
	if (is_adopted_directory(slot)) {
		std::error_code ignored;
		std::filesystem::remove_all(path_, ignored);
	}
	slot.published.store(false, std::memory_order_release);
	slot.claimed.store(false, std::memory_order_release);
}

void DirectoryPath::adopt(const std::filesystem::path &name)
{
	const auto directory = path_.string();
	const auto entry = (path_ / name).string();
	if (entry.size() >= PATH_MAX)
		throw std::runtime_error("directory path is too long: " + entry);
	struct stat status {};
	if (::lstat(path_.c_str(), &status) != 0 || !S_ISDIR(status.st_mode))
		throw std::runtime_error(
			"created directory changed unexpectedly: " + directory);
	for (std::size_t index = 0; index < maximum_directories; ++index) {
		auto &slot = directories[index];
		bool unclaimed = false;
		if (!slot.claimed.compare_exchange_strong(unclaimed, true,
			    std::memory_order_acq_rel))
			continue;
		std::memcpy(slot.path.data(), directory.c_str(),
			directory.size() + 1U);
		std::memcpy(slot.entry.data(), entry.c_str(), entry.size() + 1U);
		slot.device = status.st_dev;
		slot.inode = status.st_ino;
		slot.published.store(true, std::memory_order_release);
		slot_ = static_cast<int>(index);
		return;
	}
	throw std::runtime_error(
		"no free directory cleanup slot for " + directory);
}

std::string user_temporary_directory()
{
	const auto length = ::confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
	if (length <= 1U)
		return {};
	std::string directory(length, '\0');
	if (::confstr(_CS_DARWIN_USER_TEMP_DIR, directory.data(), length) !=
	    length)
		return {};
	directory.resize(length - 1U);
	return directory;
}

void remove_registered_socket_paths() noexcept
{
	for (const auto &slot : table)
		if (slot.published.load(std::memory_order_acquire))
			remove_slot(slot);
	for (const auto &slot : directories)
		if (slot.published.load(std::memory_order_acquire))
			remove_directory_slot(slot);
}

void install_socket_path_signal_cleanup()
{
	constexpr std::array signals{SIGHUP, SIGINT, SIGQUIT, SIGTERM};
	for (const int signal : signals) {
		struct sigaction existing {};
		if (::sigaction(signal, nullptr, &existing) != 0)
			throw std::runtime_error(
				"reading a termination disposition failed");
		/* nohup and job-control setups deliberately ignore some of
		 * these; honour that rather than making them fatal again. */
		if (existing.sa_handler == SIG_IGN)
			continue;
		struct sigaction action {};
		action.sa_handler = handle_termination;
		sigemptyset(&action.sa_mask);
		action.sa_flags = 0;
		if (::sigaction(signal, &action, nullptr) != 0)
			throw std::runtime_error(
				"installing socket-path cleanup failed");
	}
}

} // namespace uml::device
