// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_console.hpp"

#include "sockets/socket_paths.hpp"

#include <TargetConditionals.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace uml::device {

namespace {

/* arch/um/drivers/mconsole.h. The guest refuses anything else. */
constexpr std::uint32_t consoleMagic = 0xcafebabeU;
constexpr std::uint32_t consoleVersion = 2U;
constexpr std::size_t maximumConsoleData = 512U;

struct Request final {
	std::uint32_t magic;
	std::uint32_t version;
	std::uint32_t length;
	std::array<char, maximumConsoleData> data;
};

struct Reply final {
	std::uint32_t error;
	std::uint32_t more;
	std::uint32_t length;
	std::array<char, maximumConsoleData> data;
};

enum class ExchangeResult {
	notSent,
	pending,
	accepted,
	rejected,
};

/* The guest kernel prints this once its filesystem writeback has begun. Its
 * absence means the request reached a console that refused to act on it. */
constexpr std::string_view writebackAcknowledgement = "Emergency Sync";
constexpr std::string_view suspendAcknowledgement = "suspended";
/* A guest reports a suspend it gave up with "Suspend failed: <errno>". EBUSY
 * (16) is the one a moment later can cure: a wake source fired while the guest
 * synced its filesystems, or another power transition held the lock. */
constexpr std::string_view suspendBusy = "Suspend failed: -16";
/* A busy suspend is asked for again at this interval, for this long after the
 * first refusal, and then given up. */
constexpr std::chrono::milliseconds suspendRetryInterval {250};
constexpr std::chrono::milliseconds suspendRetryWindow {
	std::chrono::seconds {2}};

/* The guest answers most requests at once, so one that has no reply within
 * replyWait is left pending. It answers a suspend only once it has synced its
 * filesystems and reached the sleep boundary, or given the suspend up, which
 * can take longer: a guest with much dirty data syncs for seconds, and one
 * whose own wake sources kept ending the suspend has entered it several times.
 * The whole suspend, the first request and the busy retries after it, has one
 * deadline from the first send. The wait ends with the reply, so only such a
 * guest spends it, and it leaves most of the time iOS grants an application it
 * backgrounds. A suspend with no reply by then is aborted, not trusted: a
 * guest whose freezer is held keeps trying for 80 s and then fails. */
constexpr std::chrono::milliseconds replyWait {std::chrono::seconds {2}};
constexpr std::chrono::milliseconds suspendDeadline {std::chrono::seconds {10}};
/* A busy retry whose reply could not arrive before the deadline is not sent. */
constexpr std::chrono::milliseconds suspendRetryGuard {
	std::chrono::seconds {1}};

/* A reply socket's identifier is this many hexadecimal digits, so the widest
 * reply path is known before any request is sent. */
constexpr std::size_t clientIdDigits = 8U;
constexpr std::size_t socketPathLimit = sizeof(sockaddr_un {}.sun_path);

/* arch/um/os-Darwin/umid.c names the console <uml_dir>/mconsole. */
constexpr std::string_view consoleName = "mconsole";
/* A run's directory is uml-<umid>, and the umid is kern<pid>. */
constexpr std::string_view runDirectoryPrefix = "uml-";
constexpr std::string_view umidPrefix = "kern";

/* An iOS device application owns its temporary directory and uses it
 * directly; everywhere else a run makes a directory of its own. */
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
constexpr bool ownRunDirectory = false;
#else
constexpr bool ownRunDirectory = true;
#endif

std::mutex consoleMutex;
bool consoleAdopted;
std::unique_ptr<SocketPath> consoleGuard;
std::unique_ptr<DirectoryPath> runDirectoryGuard;
std::atomic_uint32_t nextClientId;

/* One console per process. Two guests in one process cannot both own the
 * kernel's single console anyway, and embedded runs are serialized. */
[[nodiscard]] const std::string &consoleUmid()
{
	static const std::string umid =
		std::string {umidPrefix} + std::to_string(::getpid());
	return umid;
}

/* Where this process's console goes, and the directory named in a report
 * when no location fits. */
[[nodiscard]] std::optional<GuestConsoleLocation> composeProcessLocation(
	std::string &described)
{
	const char *temporary = std::getenv("TMPDIR");
	described = temporary == nullptr ? "" : temporary;
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
	return compose_guest_console_location(described);
#else
	const std::array<std::string, 3> parents {
		described, user_temporary_directory(), "/tmp"};
	described = "$TMPDIR, " + parents[1] + " or /tmp";
	return compose_run_console_location(parents, consoleUmid());
#endif
}

/* The pid a run's name, uml-kern<pid>, records, if `name` is one. */
[[nodiscard]] std::optional<pid_t> runDirectoryOwner(std::string_view name)
{
	if (!name.starts_with(runDirectoryPrefix))
		return std::nullopt;
	name.remove_prefix(runDirectoryPrefix.size());
	if (!name.starts_with(umidPrefix))
		return std::nullopt;
	name.remove_prefix(umidPrefix.size());
	pid_t owner = 0;
	const auto [end, error] =
		std::from_chars(name.data(), name.data() + name.size(), owner);
	if (name.starts_with('0') || error != std::errc {} ||
	    end != name.data() + name.size() || owner <= 0)
		return std::nullopt;
	return owner;
}

/*
 * A run killed outright leaves its directory behind, so each run removes what
 * runs that have exited left in `parent`: directories named uml-kern<pid> that
 * this user owns, whose process no longer exists. A live process, or one this
 * user may not signal, keeps its directory, and nothing else is touched.
 */
void removeAbandonedRunDirectories(const std::filesystem::path &parent)
{
	std::error_code error;
	for (std::filesystem::directory_iterator entry {parent, error};
	     !error && entry != std::filesystem::directory_iterator {};
	     entry.increment(error)) {
		const auto filename = entry->path().filename();
		const auto owner = runDirectoryOwner(filename.native());
		if (!owner || *owner == ::getpid())
			continue;
		struct stat details {};
		if (::lstat(entry->path().c_str(), &details) != 0 ||
		    !S_ISDIR(details.st_mode) || details.st_uid != ::geteuid())
			continue;
		if (::kill(*owner, 0) == 0 || errno != ESRCH)
			continue;
		std::error_code ignored;
		std::filesystem::remove_all(entry->path(), ignored);
	}
}

/*
 * Creates the run's directory, readable by this user alone because the console
 * accepts requests that stop the guest. One this user owns already at the name
 * was left by an earlier process with this pid, and is replaced.
 */
[[nodiscard]] bool createRunDirectory(const std::string &directory)
{
	if (::mkdir(directory.c_str(), S_IRWXU) == 0)
		return true;
	if (errno != EEXIST)
		return false;
	struct stat details {};
	if (::lstat(directory.c_str(), &details) != 0 ||
	    !S_ISDIR(details.st_mode) || details.st_uid != ::geteuid())
		return false;
	std::error_code error;
	std::filesystem::remove_all(directory, error);
	return !error && ::mkdir(directory.c_str(), S_IRWXU) == 0;
}

void removeConsoleSocket(const GuestConsoleLocation &location)
{
	struct stat details {};
	if (::lstat(location.console.c_str(), &details) == 0 &&
	    S_ISSOCK(details.st_mode))
		(void)::unlink(location.console.c_str());
}

void releaseConsoleGuard()
{
	consoleGuard.reset();
	consoleAdopted = false;
}

[[nodiscard]] bool fillAddress(sockaddr_un &address, const std::string &path)
{
	if (path.size() >= sizeof(address.sun_path))
		return false;
	address = {};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
	return true;
}

/*
 * The reply comes back to the sender's address, so the client has to be bound.
 * Darwin does not autobind a Unix datagram socket, so this binds next to the
 * guest's console. Each request binds a new name, so a reply that arrives
 * after its request gave up finds no socket instead of the next request's. A
 * reply that has not arrived within a receive's wait is left pending, and the
 * client may receive again: a request that matters past its wait keeps its
 * client, and with it the socket, for as long as it is read.
 */
class Client final {
public:
	explicit Client(const GuestConsoleLocation &location)
	{
		std::array<char, clientIdDigits + 1U> id {};
		(void)std::snprintf(id.data(), id.size(), "%08x",
			static_cast<unsigned int>(nextClientId.fetch_add(
				1U, std::memory_order_relaxed)));
		path_ = location.client_prefix + id.data();
		(void)::unlink(path_.c_str());
		if (!fillAddress(console_, location.console))
			return;
		descriptor_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
		if (descriptor_ < 0)
			return;
		sockaddr_un address {};
		if (!fillAddress(address, path_) ||
		    ::bind(descriptor_, reinterpret_cast<sockaddr *>(&address),
			    sizeof(address)) != 0) {
			::close(descriptor_);
			descriptor_ = -1;
			return;
		}
		/* A terminating signal while the request waits would otherwise
		 * leave the reply socket, and with it the run's directory. */
		try {
			auto guard = std::make_unique<SocketPath>(path_);
			guard->adopt();
			guard_ = std::move(guard);
		} catch (...) {
		}
	}

	~Client()
	{
		if (descriptor_ >= 0)
			::close(descriptor_);
		guard_.reset();
		(void)::unlink(path_.c_str());
	}

	Client(const Client &) = delete;
	Client &operator=(const Client &) = delete;

	[[nodiscard]] bool usable() const noexcept { return descriptor_ >= 0; }

	/* False when nothing was sent. */
	[[nodiscard]] bool send(std::string_view command) const noexcept
	{
		Request request {consoleMagic, consoleVersion,
			static_cast<std::uint32_t>(command.size()), {}};
		if (!usable() || command.size() >= request.data.size())
			return false;
		std::memcpy(request.data.data(), command.data(), command.size());
		const auto sent = ::sendto(descriptor_, &request, sizeof(request), 0,
			reinterpret_cast<const sockaddr *>(&console_),
			sizeof(console_));
		return sent == static_cast<ssize_t>(sizeof(request));
	}

	/* Collects the reply to the request, waiting up to `wait` for each part
	 * of it. A zero wait only takes what has arrived. */
	[[nodiscard]] ExchangeResult receive(std::string &response,
		std::chrono::milliseconds wait) const
	{
		int flags = 0;
		if (wait.count() <= 0) {
			flags = MSG_DONTWAIT;
		} else {
			const timeval timeout {
				.tv_sec = static_cast<decltype(timeval::tv_sec)>(
					wait.count() / 1000),
				.tv_usec = static_cast<decltype(timeval::tv_usec)>(
					wait.count() % 1000 * 1000)};
			(void)::setsockopt(descriptor_, SOL_SOCKET, SO_RCVTIMEO,
				&timeout, sizeof(timeout));
		}
		for (;;) {
			Reply reply {};
			const auto received = ::recv(descriptor_, &reply, sizeof(reply),
				flags);
			if (received < static_cast<ssize_t>(sizeof(reply) -
				    maximumConsoleData))
				return ExchangeResult::pending;
			auto length = std::min(
				static_cast<std::size_t>(reply.length),
				maximumConsoleData);
			/* The guest's driver counts the NUL that ends each reply in
			 * the reply's length. It ends the text and is no part of it. */
			if (length != 0U && reply.data[length - 1U] == '\0')
				--length;
			response.append(reply.data.data(), length);
			if (reply.more == 0U)
				return reply.error == 0U ? ExchangeResult::accepted :
					ExchangeResult::rejected;
		}
	}

private:
	int descriptor_ {-1};
	std::string path_;
	sockaddr_un console_ {};
	std::unique_ptr<SocketPath> guard_;
};

/*
 * The guest created this socket, so it can only be adopted once it exists.
 * The location belongs to this process, which makes anything at that path
 * ours; tightening the mode keeps the guest's console away from other local
 * users, because a console accepts requests that stop the guest.
 */
void adoptConsolePath(const std::string &path)
{
	if (consoleAdopted)
		return;
	struct stat details {};
	if (::lstat(path.c_str(), &details) != 0 || !S_ISSOCK(details.st_mode))
		return;
	(void)::chmod(path.c_str(), S_IRUSR | S_IWUSR);
	try {
		auto guard = std::make_unique<SocketPath>(path);
		guard->adopt();
		consoleGuard = std::move(guard);
		consoleAdopted = true;
	} catch (...) {
		/* Cleanup on a terminating signal is a convenience; failing to
		 * arrange it must not fail the writeback request itself. */
	}
}

/* Sends one command to the location's console from a client of its own and
 * collects the reply. */
[[nodiscard]] ExchangeResult exchange(
	const GuestConsoleLocation &location,
	std::string_view command,
	std::string &response,
	std::chrono::milliseconds wait = replyWait)
{
	const Client client {location};
	if (!client.send(command))
		return ExchangeResult::notSent;
	return client.receive(response, wait);
}

[[nodiscard]] bool requestWriteback(const GuestConsoleLocation &location,
	unsigned int settleMs)
{
	std::string response;
	if (exchange(location, "sysrq s", response) != ExchangeResult::accepted)
		return false;
	if (response.find(writebackAcknowledgement) == std::string::npos) {
		std::fputs(
			"Guest console refused the filesystem writeback; "
			"the guest has magic SysRq disabled\n",
			stderr);
		return false;
	}
	/* The guest reports that the writeback started, not that it finished, so
	 * give it a bounded chance to reach the block layer before the host flushes
	 * what it received. */
	if (settleMs != 0U) {
		std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
	}
	return true;
}

} // namespace

std::optional<GuestConsoleLocation> compose_guest_console_location(
	std::string_view directory)
{
	/* arch/um/os-Darwin/umid.c composes <uml_dir>/<name> after dropping the
	 * same trailing slashes. */
	while (directory.size() > 1U && directory.ends_with('/'))
		directory.remove_suffix(1U);
	if (!directory.starts_with('/'))
		return std::nullopt;
	GuestConsoleLocation location;
	location.directory = directory;
	location.console =
		location.directory + "/" + std::string {consoleName};
	location.client_prefix = location.directory + "/mc-";
	if (location.console.size() >= socketPathLimit ||
	    location.client_prefix.size() + clientIdDigits >= socketPathLimit)
		return std::nullopt;
	return location;
}

std::optional<GuestConsoleLocation> compose_run_console_location(
	std::span<const std::string> parents, std::string_view umid)
{
	for (std::string_view parent : parents) {
		while (parent.size() > 1U && parent.ends_with('/'))
			parent.remove_suffix(1U);
		if (!parent.starts_with('/'))
			continue;
		auto directory = std::string {parent};
		if (directory != "/")
			directory += '/';
		directory += runDirectoryPrefix;
		directory += umid;
		if (auto location = compose_guest_console_location(directory))
			return location;
	}
	return std::nullopt;
}

const std::optional<GuestConsoleLocation> &guest_console_location()
{
	static const std::optional<GuestConsoleLocation> location = [] {
		std::string described;
		auto composed = composeProcessLocation(described);
		if (!composed) {
			std::fprintf(stderr,
				"Guest management console: no console under %s: "
				"it needs an absolute directory whose socket paths fit "
				"in %zu bytes; the host cannot reach the guest's console\n",
				described.c_str(), socketPathLimit);
		}
		return composed;
	}();
	return location;
}

std::vector<std::string> guest_management_console_arguments()
{
	std::vector<std::string> arguments {"umid=" + consoleUmid()};
	if (const auto &location = guest_console_location())
		arguments.push_back("uml_dir=" + location->directory);
	return arguments;
}

bool request_guest_writeback(unsigned int settleMs) noexcept
{
	try {
		std::lock_guard lock(consoleMutex);
		const auto &location = guest_console_location();
		if (!location)
			return false;
		adoptConsolePath(location->console);
		return requestWriteback(*location, settleMs);
	} catch (...) {
		return false;
	}
}

namespace {

/* A guest that accepted a suspend: it said it reached the sleep boundary. */
[[nodiscard]] GuestSuspension suspendAccepted(const std::string &response)
{
	if (response.find(suspendAcknowledgement) == std::string::npos) {
		std::fputs("Guest console returned an unexpected suspend reply\n",
			stderr);
	}
	return GuestSuspension::system;
}

/* A guest that refused a suspend, or kept refusing. It runs on and the caller
 * does not treat it as asleep: the console's own stop would halt only the
 * vCPU that takes it, since the kernel blocks signals per vCPU, so it is no
 * substitute for a suspend. */
[[nodiscard]] GuestSuspension suspendRefused(
	const GuestConsoleLocation &location, const std::string &response,
	unsigned int writebackSettleMs)
{
	std::fprintf(stderr,
		"Guest console: the guest could not suspend; it keeps running "
		"(%s)\n", response.c_str());
	/* A suspend that holds performs the sync itself and avoids spending the
	 * iOS background budget twice; a guest that refused it has not synced, so
	 * write its filesystems back for the host's flush. */
	(void)requestWriteback(location, writebackSettleMs);
	return GuestSuspension::none;
}

/*
 * A suspend that has not replied by its deadline is aborted, not trusted. A
 * guest whose freezer cannot finish keeps trying for tens of seconds and then
 * fails, so a host that took the silence for a suspended guest would
 * hibernate one that runs. The wake edge is an interrupt command that the
 * guest answers while its suspend blocks, and it makes the PM core give an
 * attempt up before the devices are touched. The request's own socket, kept
 * for this, then carries the guest's final word.
 *
 * Only a reply that arrived before the wake proves the suspend: the guest
 * replies "suspended" before it checks for a wakeup, so one that follows the
 * wake does not say the guest slept. A refusal is a guest known to run;
 * anything else is unconfirmed, and its resume wakes it.
 */
[[nodiscard]] GuestSuspension abortSuspend(
	const GuestConsoleLocation &location, const Client &request,
	unsigned int writebackSettleMs)
{
	std::string response;
	switch (request.receive(response, std::chrono::milliseconds {0})) {
	case ExchangeResult::accepted:
		return suspendAccepted(response);
	case ExchangeResult::rejected:
		return suspendRefused(location, response, writebackSettleMs);
	case ExchangeResult::notSent:
	case ExchangeResult::pending:
		break;
	}
	std::string woken;
	(void)exchange(location, "wake", woken);
	response.clear();
	const auto result = request.receive(response, replyWait);
	const bool refused = result == ExchangeResult::rejected;
	std::fprintf(stderr,
		"Guest console: suspend did not finish within %lld s; woken, it "
		"keeps running (final reply: %s)\n",
		static_cast<long long>(
			std::chrono::duration_cast<std::chrono::seconds>(
				suspendDeadline).count()),
		result == ExchangeResult::pending ? "none" : response.c_str());
	(void)requestWriteback(location, writebackSettleMs);
	return refused ? GuestSuspension::none : GuestSuspension::unconfirmed;
}

} // namespace

GuestSuspension suspend_guest(unsigned int writebackSettleMs) noexcept
{
	try {
		std::lock_guard lock(consoleMutex);
		const auto &location = guest_console_location();
		if (!location)
			return GuestSuspension::none;
		adoptConsolePath(location->console);
		const auto deadline =
			std::chrono::steady_clock::now() + suspendDeadline;
		/* A suspend-capable guest replies only after devices, secondary
		 * CPUs, interrupts, and timekeeping have reached the platform sleep
		 * boundary. The separate wake request remains queued on the console
		 * descriptor until the kernel has resumed it. The request that is
		 * waiting for its reply keeps its socket, which a timeout then
		 * reads again. */
		std::unique_ptr<Client> request;
		std::string response;
		auto result = ExchangeResult::notSent;
		const auto ask = [&] {
			response.clear();
			request = std::make_unique<Client>(*location);
			if (!request->send("suspend")) {
				result = ExchangeResult::notSent;
				return;
			}
			result = request->receive(response,
				std::chrono::duration_cast<std::chrono::milliseconds>(
					deadline - std::chrono::steady_clock::now()));
		};
		ask();
		/* A guest that was busy, as when a wake source fired while it synced,
		 * is asked again for a short while, if its answer could still arrive
		 * by the deadline. The refusal ended the guest's suspend, so no
		 * request of ours is still in flight. */
		std::chrono::steady_clock::time_point retryDeadline;
		bool retrying = false;
		while (result == ExchangeResult::rejected &&
		       response.starts_with(suspendBusy)) {
			if (!retrying) {
				retryDeadline = std::chrono::steady_clock::now() +
					suspendRetryWindow;
				retrying = true;
			}
			std::this_thread::sleep_for(suspendRetryInterval);
			const auto now = std::chrono::steady_clock::now();
			if (now >= retryDeadline || deadline - now <= suspendRetryGuard)
				break;
			ask();
		}
		switch (result) {
		case ExchangeResult::accepted:
			return suspendAccepted(response);
		case ExchangeResult::notSent:
			return GuestSuspension::none;
		case ExchangeResult::rejected:
			/* The guest refused, or kept refusing. */
			return suspendRefused(*location, response, writebackSettleMs);
		case ExchangeResult::pending:
			return abortSuspend(*location, *request, writebackSettleMs);
		}
		return GuestSuspension::none;
	} catch (...) {
		return GuestSuspension::none;
	}
}

void resume_guest(GuestSuspension suspension) noexcept
{
	if (suspension == GuestSuspension::none)
		return;
	try {
		std::lock_guard lock(consoleMutex);
		const auto &location = guest_console_location();
		if (!location)
			return;
		std::string response;
		(void)exchange(*location, "wake", response);
	} catch (...) {
	}
}

namespace {

[[nodiscard]] GuestRequest requestGuest(std::string_view command,
	std::chrono::milliseconds wait) noexcept
{
	try {
		std::lock_guard lock(consoleMutex);
		const auto &location = guest_console_location();
		if (!location)
			return GuestRequest::unreachable;
		adoptConsolePath(location->console);
		std::string response;
		switch (exchange(*location, command, response, wait)) {
		case ExchangeResult::notSent:
			return GuestRequest::unreachable;
		case ExchangeResult::pending:
			return GuestRequest::unanswered;
		case ExchangeResult::rejected:
			return GuestRequest::refused;
		case ExchangeResult::accepted:
			return GuestRequest::done;
		}
		return GuestRequest::refused;
	} catch (...) {
		return GuestRequest::unreachable;
	}
}

} // namespace

bool wake_guest() noexcept
{
	return requestGuest("wake", replyWait) == GuestRequest::done;
}

GuestRequest set_guest_page_cache_trim(bool enabled) noexcept
{
	return requestGuest(enabled ? "trim on" : "trim off", replyWait);
}

GuestRequest trim_guest_page_cache(unsigned int waitMs) noexcept
{
	/* A zero receive timeout would wait for ever. */
	return requestGuest("trim once",
		std::chrono::milliseconds {std::max(waitMs, 1U)});
}

GuestConsoleRun::GuestConsoleRun(bool active) noexcept
	: active_(active)
{
	if (!active_)
		return;
	try {
		std::lock_guard lock(consoleMutex);
		const auto &location = guest_console_location();
		if (!location)
			return;
		releaseConsoleGuard();
		if constexpr (!ownRunDirectory) {
			removeConsoleSocket(*location);
			return;
		}
		runDirectoryGuard.reset();
		const std::filesystem::path directory {location->directory};
		removeAbandonedRunDirectories(directory.parent_path());
		if (!createRunDirectory(location->directory)) {
			std::fprintf(stderr,
				"Guest management console: cannot create %s: %s; "
				"the host cannot reach the guest's console\n",
				location->directory.c_str(), std::strerror(errno));
			return;
		}
		auto guard = std::make_unique<DirectoryPath>(directory);
		try {
			guard->adopt(std::string {consoleName});
		} catch (...) {
			/* Never a directory that a terminating signal would leave. */
			std::error_code ignored;
			std::filesystem::remove_all(directory, ignored);
			throw;
		}
		runDirectoryGuard = std::move(guard);
	} catch (...) {
	}
}

GuestConsoleRun::~GuestConsoleRun()
{
	if (!active_)
		return;
	try {
		std::lock_guard lock(consoleMutex);
		releaseConsoleGuard();
		if (const auto &location = guest_console_location())
			removeConsoleSocket(*location);
		runDirectoryGuard.reset();
	} catch (...) {
	}
}


namespace {

/* Whether `text` is a count of decimal digits that fits 64 bits, and the
 * count. No sign, no padding. */
[[nodiscard]] bool parseByteCount(std::string_view text,
	std::uint64_t &count) noexcept
{
	if (text.empty())
		return false;
	const auto [end, error] =
		std::from_chars(text.data(), text.data() + text.size(), count);
	return error == std::errc {} && end == text.data() + text.size();
}

} // namespace

std::optional<GuestMemoryFigures> parse_guest_memory_reply(
	std::string_view text) noexcept
{
	if (text.ends_with('\n'))
		text.remove_suffix(1U);
	std::optional<std::uint64_t> total;
	std::optional<std::uint64_t> available;
	std::optional<std::uint64_t> free;
	for (;;) {
		const auto separator = text.find(' ');
		const auto field = text.substr(0U, separator);
		const auto equals = field.find('=');
		if (equals == std::string_view::npos || equals == 0U)
			return std::nullopt;
		const auto key = field.substr(0U, equals);
		std::uint64_t count = 0U;
		if (!parseByteCount(field.substr(equals + 1U), count))
			return std::nullopt;
		std::optional<std::uint64_t> *named = nullptr;
		if (key == "total")
			named = &total;
		else if (key == "available")
			named = &available;
		else if (key == "free")
			named = &free;
		if (named != nullptr) {
			if (named->has_value())
				return std::nullopt;
			*named = count;
		}
		if (separator == std::string_view::npos)
			break;
		text.remove_prefix(separator + 1U);
	}
	if (!total || !available)
		return std::nullopt;
	return GuestMemoryFigures {.total_bytes = *total,
		.available_bytes = *available,
		.free_bytes = free.value_or(0U)};
}

class GuestMemoryQuery::State final {
public:
	explicit State(const GuestConsoleLocation &location) : client(location) {}

	Client client;
};

GuestMemoryQuery::GuestMemoryQuery(std::unique_ptr<State> state) noexcept
	: state_(std::move(state))
{
}

GuestMemoryQuery::GuestMemoryQuery(GuestMemoryQuery &&other) noexcept = default;

GuestMemoryQuery::~GuestMemoryQuery() = default;

std::optional<GuestMemoryQuery> GuestMemoryQuery::start() noexcept
{
	try {
		/* The console's lock covers locating it and sending, not the wait
		 * for the reply, which the caller makes without it. */
		std::lock_guard lock(consoleMutex);
		const auto &location = guest_console_location();
		if (!location)
			return std::nullopt;
		adoptConsolePath(location->console);
		auto state = std::make_unique<State>(*location);
		if (!state->client.send("mem"))
			return std::nullopt;
		return GuestMemoryQuery(std::move(state));
	} catch (...) {
		return std::nullopt;
	}
}

GuestMemoryResult GuestMemoryQuery::await(std::chrono::milliseconds wait,
	GuestMemoryFigures &figures) noexcept
{
	try {
		std::string response;
		switch (state_->client.receive(response, wait)) {
		case ExchangeResult::notSent:
			return GuestMemoryResult::unreachable;
		case ExchangeResult::pending:
			return GuestMemoryResult::unanswered;
		case ExchangeResult::rejected:
			return GuestMemoryResult::refused;
		case ExchangeResult::accepted:
			break;
		}
		const auto parsed = parse_guest_memory_reply(response);
		if (!parsed)
			return GuestMemoryResult::malformed;
		figures = *parsed;
		return GuestMemoryResult::done;
	} catch (...) {
		return GuestMemoryResult::unanswered;
	}
}

} // namespace uml::device
