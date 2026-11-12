// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uml::device {

enum class GuestSuspension {
	/* The guest runs, or no request reached it. A resume sends nothing. */
	none,
	/* The guest replied that it entered system suspend. */
	system,
	/* The guest may be suspended or running: the suspend did not reply within
	 * its deadline, was aborted with a wake, and gave no final word. A resume
	 * sends the wake. */
	unconfirmed,
};

/*
 * UML's own management console is the only channel that reaches the guest
 * kernel while it runs, and the only way to make it write back pages it has
 * not yet handed to the block layer. Everything the host can flush by itself
 * stops at the block device.
 *
 * The guest binds the console as a datagram socket and answers each request at
 * the address it came from, so the host binds one reply socket per request.
 * Both live in a directory that belongs to this process alone: the guest
 * receives it as uml_dir= and binds <directory>/mconsole, and a request binds
 * <directory>/mc-<id>. Nothing goes to the shared /tmp, where a guest's
 * socket outlived every process killed before its guest shut down.
 */
struct GuestConsoleLocation final {
	/* Handed to the guest as uml_dir=. */
	std::string directory;
	std::string console;
	/* A request's reply socket appends eight hexadecimal digits. */
	std::string client_prefix;
};

/*
 * Composes a location, dropping the directory's trailing slashes as the guest
 * does. Every path it can name, the guest's and the widest reply socket's,
 * must fit sun_path. A directory that is not absolute, or for which one of
 * them would not fit, yields nothing: never a truncated path.
 */
[[nodiscard]] std::optional<GuestConsoleLocation>
compose_guest_console_location(std::string_view directory);

/*
 * A run's own directory, uml-<umid>, in the first of `parents` that is
 * absolute and leaves every path short enough. Nothing when none does.
 */
[[nodiscard]] std::optional<GuestConsoleLocation> compose_run_console_location(
	std::span<const std::string> parents, std::string_view umid);

/*
 * This process's location. The iOS sandbox refuses sockets outside the
 * application, so an iOS device uses its own temporary directory, $TMPDIR,
 * directly, and the application owns everything in it. macOS and the iOS
 * Simulator make uml-kern<pid> in $TMPDIR, or in the user's temporary
 * directory or /tmp when $TMPDIR is unset or too long for sun_path, as the
 * Simulator's application directory is. Nothing, reported once on stderr,
 * leaves the guest without a console the host can reach.
 */
[[nodiscard]] const std::optional<GuestConsoleLocation> &
guest_console_location();

/*
 * The kernel arguments that put the guest's console where the host looks for
 * it: umid= and uml_dir=. Every composition root passes them. A caller that
 * supplies uml_dir= keeps its own directory, which the host then neither
 * prepares nor removes and cannot reach.
 */
[[nodiscard]] std::vector<std::string> guest_management_console_arguments();

/*
 * Asks the guest to write its filesystems back and waits for it to accept the
 * request, then allows a bounded settling period for the writeback itself.
 *
 * True means the guest confirmed the request. It is not a completion proof:
 * the guest schedules the writeback and reports that it started, so the
 * settling period is a bound rather than a guarantee. False means no console
 * answered, which is the normal result where the guest could not bind one.
 */
[[nodiscard]] bool request_guest_writeback(unsigned int settle_ms) noexcept;

/*
 * Enters Linux system suspend, so devices, secondary CPUs, interrupts, and
 * timekeeping stop at a kernel-owned boundary, and sends the explicit wake
 * edge. Every maintained kernel has system suspend; the management console's
 * own stop halts only the vCPU that takes it, so nothing replaces a suspend
 * that fails.
 *
 * The returned mode records whether the wake request is required. The guest
 * replies to a suspend request once it has synced its filesystems, so the
 * whole request, its busy retries included, has one ten-second deadline from
 * the first send. A guest that reports itself busy (EBUSY) is asked again every
 * 250 ms for two seconds while the deadline leaves time for the answer. Any
 * other refusal, or a busy guest that stays busy, logs that the guest keeps
 * running, writes its filesystems back, and returns none.
 *
 * A request with no reply at the deadline is aborted, not trusted, because a
 * guest whose freezer is held fails its suspend tens of seconds later. The
 * request's socket stays open, a wake is sent, and the final reply is read for
 * two seconds more, then the filesystems are written back. A reply that
 * arrived before the wake decides: "suspended" is system. After the wake, a
 * refusal is none (the guest is known to run), and "suspended" or silence is
 * unconfirmed. None also means no request reached the console. Resuming is
 * best effort and reports nothing, wakes a system or unconfirmed guest, and
 * sends nothing for none.
 */
[[nodiscard]] GuestSuspension suspend_guest(
	unsigned int writeback_settle_ms) noexcept;
void resume_guest(GuestSuspension suspension) noexcept;

/*
 * Sends the wake edge whatever the host last asked for, as a phone's power
 * key does: it ends a system suspend the guest is entering or entered by
 * itself, and a running guest only answers it. True means the guest
 * answered.
 */
bool wake_guest() noexcept;

/* How a request to the guest's console ended. */
enum class GuestRequest {
	/* No console was reachable, so nothing was sent. */
	unreachable,
	/* Sent, but no reply came within the bound. The guest may still act. */
	unanswered,
	/* The guest refused it, as a kernel without the command does. */
	refused,
	/* The guest did it. */
	done,
};

/*
 * Turns the guest's periodic page-cache trimming on or off ("trim on",
 * "trim off"). A guest starts with it off. While it is on, the guest hands
 * its cold page cache to the host every few seconds, backing off while it
 * reads evicted pages back.
 */
[[nodiscard]] GuestRequest set_guest_page_cache_trim(bool enabled) noexcept;

/*
 * Has the guest trim all of its cold page cache and hand its free memory to
 * the host now ("trim once"), waiting up to `wait_ms` for it to finish. Like
 * every console request it wakes a suspended guest, so send it before a
 * suspend, never during one.
 */
[[nodiscard]] GuestRequest trim_guest_page_cache(unsigned int wait_ms) noexcept;

/*
 * The guest kernel's own memory figures, in bytes, as its management console
 * reports them for "mem": MemTotal, MemAvailable and MemFree, the counters
 * /proc/meminfo prints. They are read without a file, so a guest whose policy
 * keeps the console from /proc/meminfo answers all the same.
 */
struct GuestMemoryFigures final {
	std::uint64_t total_bytes {};
	std::uint64_t available_bytes {};
	std::uint64_t free_bytes {};
};

/*
 * The reply to "mem" is one line, "total=<n> available=<n> free=<n>": decimal
 * byte counts, one space between fields, no sign and no padding. Another
 * field of the form key=<n> after them is ignored. Nothing when a count is
 * missing, repeated, signed or too large for 64 bits, when `total` or
 * `available` is absent, or when anything else is on the line; one trailing
 * line end is allowed. `free` is informational, so a reply without it
 * reads as zero free bytes.
 */
[[nodiscard]] std::optional<GuestMemoryFigures> parse_guest_memory_reply(
	std::string_view text) noexcept;

/* How a "mem" request ended. */
enum class GuestMemoryResult {
	/* No console was reachable, so nothing was sent. */
	unreachable,
	/* Sent, but no reply came within the bound. */
	unanswered,
	/* The guest refused it, as a kernel without the command does. */
	refused,
	/* The guest answered with something that is not the figures. */
	malformed,
	/* The guest answered with its figures. */
	done,
};

/*
 * A "mem" request that was sent and waits for its reply, in two steps so that
 * a caller can send it while it holds a lock and wait for the reply without
 * the lock: the guest answers from its interrupt at once, but a guest that
 * does not would otherwise hold the lock for the whole wait. The request owns
 * its reply socket until it is destroyed, and a reply that comes after that
 * finds no socket.
 *
 * Like every console request it wakes a guest that is suspended, so send it
 * only to a guest known to run.
 */
class GuestMemoryQuery final {
public:
	/* Sends the request and returns at once. Nothing when no console is
	 * reachable or the datagram could not be sent. */
	[[nodiscard]] static std::optional<GuestMemoryQuery> start() noexcept;

	GuestMemoryQuery(GuestMemoryQuery &&other) noexcept;
	GuestMemoryQuery &operator=(GuestMemoryQuery &&) = delete;
	GuestMemoryQuery(const GuestMemoryQuery &) = delete;
	GuestMemoryQuery &operator=(const GuestMemoryQuery &) = delete;
	~GuestMemoryQuery();

	/* Waits up to `wait` for the reply, and fills `figures` when it is
	 * done. A zero wait takes only a reply that has already arrived. May be
	 * called again after `unanswered`. */
	[[nodiscard]] GuestMemoryResult await(std::chrono::milliseconds wait,
		GuestMemoryFigures &figures) noexcept;

private:
	class State;
	explicit GuestMemoryQuery(std::unique_ptr<State> state) noexcept;

	std::unique_ptr<State> state_;
};

/*
 * Holds this process's console location for one guest run that passed the
 * host's uml_dir=.
 *
 * Construction readies the location before the guest binds it. On macOS and
 * the Simulator it creates the run's directory, mode 0700, published for
 * removal on every terminating signal and kernel-driven exit, and first removes
 * the directories of runs that have exited: a process killed outright runs no
 * cleanup at all. On an iOS device it removes a socket left at the fixed name,
 * which would refuse the guest's bind; a guest removes its socket only when it
 * shuts down, not when iOS ends the application.
 *
 * Destruction, when the run returns however the guest ended, removes the
 * console socket and, on macOS and the Simulator, the directory.
 */
class GuestConsoleRun final {
public:
	explicit GuestConsoleRun(bool active) noexcept;
	~GuestConsoleRun();

	GuestConsoleRun(const GuestConsoleRun &) = delete;
	GuestConsoleRun &operator=(const GuestConsoleRun &) = delete;

private:
	bool active_;
};

} // namespace uml::device
