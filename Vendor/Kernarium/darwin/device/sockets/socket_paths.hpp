// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string>

namespace uml::device {

/*
 * A bound Unix-socket path that must not outlive its process.
 *
 * The destructor removes the path on an ordinary teardown.  An adopted path is
 * additionally published to the handler installed by
 * install_socket_path_signal_cleanup(), because a terminating signal never
 * unwinds this object and would otherwise leave the path behind.  A stale path
 * is not merely untidy: the console socket server refuses to replace an
 * existing path, so the next run of the same command fails to start.
 */
class SocketPath final {
public:
	explicit SocketPath(std::filesystem::path path);
	~SocketPath();

	SocketPath(const SocketPath &) = delete;
	SocketPath &operator=(const SocketPath &) = delete;

	/* Records the bound path's device and inode and publishes it to the
	 * signal handler.  Call once, immediately after bind() succeeds. */
	void adopt();

private:
	std::filesystem::path path_;
	int slot_{-1};
};

/*
 * A directory this process created for its own use alone, which must not
 * outlive the process.
 *
 * The destructor removes an adopted directory and everything in it on an
 * ordinary teardown. Adopting it also publishes it, with the one entry it was
 * made to hold, to remove_registered_socket_paths(), because a terminating
 * signal or a kernel-driven exit never unwinds this object. That path removes
 * the entry by name, whoever created it, and then the directory. A socket
 * adopted inside the directory, such as a request's reply socket, goes first.
 */
class DirectoryPath final {
public:
	explicit DirectoryPath(std::filesystem::path path);
	~DirectoryPath();

	DirectoryPath(const DirectoryPath &) = delete;
	DirectoryPath &operator=(const DirectoryPath &) = delete;

	/* Records the directory's device and inode and publishes it, with the
	 * entry `name` inside it, to the termination path. Call once,
	 * immediately after creating the directory. */
	void adopt(const std::filesystem::path &name);

private:
	std::filesystem::path path_;
	int slot_{-1};
};

/*
 * The user's temporary directory as Darwin names it, whatever $TMPDIR says: the
 * place for a run's own host files when $TMPDIR is unset or unusable, rather
 * than the shared /tmp. Empty where there is none, as on an iOS device.
 */
[[nodiscard]] std::string user_temporary_directory();

/*
 * Removes every adopted socket path whose device and inode still match what
 * was bound, so a path another process has since recreated is left alone.
 * Then it removes every adopted directory that is still the one adopted: its
 * named entry first, then the directory, which stays if anything else is left
 * in it. Async-signal-safe: it reads preallocated storage and calls only
 * lstat(), unlink() and rmdir().
 */
void remove_registered_socket_paths() noexcept;

/*
 * Installs SIGHUP/SIGINT/SIGQUIT/SIGTERM handlers that remove adopted socket
 * paths and directories and then re-raise with the default disposition, leaving
 * the wait status a shell or supervisor observes unchanged.  A signal the caller already
 * ignores keeps that disposition.
 */
void install_socket_path_signal_cleanup();

} // namespace uml::device
