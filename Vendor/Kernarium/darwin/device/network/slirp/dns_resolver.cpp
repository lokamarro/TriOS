// SPDX-License-Identifier: GPL-2.0-or-later
#include "dns_resolver.hpp"

#include <dns_sd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

namespace uml::network {

using uml::platform::UniqueDescriptor;
using uml::platform::configureDescriptor;

/* One guest question while the system resolver works on it. */
struct HostResolver::Pending final {
	HostResolver *owner{};
	std::optional<dns::Endpoints> datagram;
	std::uint64_t stream{};
	dns::Query query;
	DNSServiceRef reference{};
	std::chrono::steady_clock::time_point deadline{};
	/* The name the next answer belongs to: the question's, then each
	 * CNAME target in turn. */
	std::vector<std::uint8_t> tail;
	unsigned int aliases{};
	std::vector<dns::Record> answers;
	std::uint16_t rcode{dns::rcode_noerror};
	/* An answer, a negative answer or a failure is in: answer at the end
	 * of the batch, once every record of the final RRset has arrived. */
	bool done{};
	/* No later record changes the answer. */
	bool closed{};
	/* Answer now, whatever the batch: the deadline passed or the daemon
	 * connection was lost. */
	bool expired{};
};

namespace {

constexpr unsigned int alias_limit = 8;
constexpr std::size_t stream_buffer_limit = 2 * (2 + dns::tcp_limit);

/* Called from DNSServiceProcessResult: nothing may unwind into its C frames. */
void DNSSD_API query_reply(DNSServiceRef, DNSServiceFlags flags,
			   uint32_t, DNSServiceErrorType error,
			   const char *fullname, uint16_t type, uint16_t klass,
			   uint16_t length, const void *data, uint32_t ttl,
			   void *context) noexcept
{
	auto *pending = static_cast<HostResolver::Pending *>(context);
	pending->owner->record(
		*pending, flags, error, fullname, type, klass,
		{static_cast<const std::uint8_t *>(data),
		 data == nullptr ? 0U : length},
		ttl);
}

bool create_pipe(std::array<UniqueDescriptor, 2> &result) noexcept
{
	std::array<int, 2> descriptors{};
	if (::pipe(descriptors.data()) != 0)
		return false;
	result[0].reset(descriptors[0]);
	result[1].reset(descriptors[1]);
	return configureDescriptor(result[0].get(),
				   {.closeOnExec = true, .nonblocking = true}) &&
		configureDescriptor(result[1].get(),
				    {.closeOnExec = true, .nonblocking = true});
}

/* How long to poll until `next`, the earliest deadline; none waits for a
 * descriptor alone, so an idle resolver wakes for nothing. */
int poll_timeout(std::chrono::steady_clock::time_point now,
		 std::chrono::steady_clock::time_point next) noexcept
{
	if (next == std::chrono::steady_clock::time_point::max())
		return -1;
	if (next <= now)
		return 0;
	const auto wait =
		std::chrono::duration_cast<std::chrono::milliseconds>(next - now)
			.count() + 1;
	return static_cast<int>(std::min<long long>(wait, 60'000));
}

/*
 * A run killed outright leaves its socket, so each run removes what runs that
 * have exited left beside its own: sockets named dns-<pid> that this user
 * owns, whose process no longer exists. A live process, or one this user may
 * not signal, keeps its socket, and nothing else is touched.
 */
void remove_abandoned_streams(const std::filesystem::path &directory) noexcept
{
	try {
		std::error_code error;
		for (std::filesystem::directory_iterator entry{directory, error};
		     !error && entry != std::filesystem::directory_iterator{};
		     entry.increment(error)) {
			const auto filename = entry->path().filename().native();
			std::string_view name{filename};
			if (!name.starts_with(dns_stream_prefix))
				continue;
			name.remove_prefix(dns_stream_prefix.size());
			pid_t owner = 0;
			const auto [end, parsed] = std::from_chars(
				name.data(), name.data() + name.size(), owner);
			if (name.starts_with('0') || parsed != std::errc{} ||
			    end != name.data() + name.size() || owner <= 0 ||
			    owner == ::getpid())
				continue;
			struct stat details {};
			if (::lstat(entry->path().c_str(), &details) != 0 ||
			    !S_ISSOCK(details.st_mode) ||
			    details.st_uid != ::geteuid())
				continue;
			if (::kill(owner, 0) == 0 || errno != ESRCH)
				continue;
			(void)::unlink(entry->path().c_str());
		}
	} catch (...) {
	}
}

} // namespace

/* Out of line: Pending is complete only here. */
HostResolver::HostResolver() = default;

HostResolver::~HostResolver()
{
	stop();
}

int HostResolver::start(FrameSink sink,
			const std::vector<std::string> &stream_paths)
{
	if (worker_.joinable())
		return -EBUSY;
	if (!create_pipe(wake_pipe_))
		return errno == 0 ? -EIO : -errno;
	sink_ = std::move(sink);
	int error = -ENOENT;
	for (const auto &path : stream_paths) {
		error = bind_listener(path);
		if (error == 0) {
			stream_path_ = path;
			remove_abandoned_streams(
				std::filesystem::path{path}.parent_path());
			break;
		}
	}
	if (error != 0)
		std::fprintf(stderr,
			"Darwin user network: DNS over TCP is unavailable "
			"(%s)\n", std::strerror(-error));
	{
		std::lock_guard lock{inbox_mutex_};
		accepting_ = true;
	}
	try {
		worker_ = std::jthread{[this](std::stop_token stop_token) {
			run(stop_token);
		}};
	} catch (const std::system_error &) {
		std::lock_guard lock{inbox_mutex_};
		accepting_ = false;
		return -EIO;
	}
	return 0;
}

void HostResolver::stop() noexcept
{
	{
		std::lock_guard lock{inbox_mutex_};
		accepting_ = false;
		inbox_.clear();
	}
	if (worker_.joinable()) {
		worker_.request_stop();
		wake();
		worker_.join();
	}
	listener_.reset();
	listener_path_.reset();
	stream_path_.clear();
	wake_pipe_[0].reset();
	wake_pipe_[1].reset();
	sink_ = nullptr;
}

void HostResolver::submit(const dns::Endpoints &endpoints,
			  std::span<const std::uint8_t> message)
{
	{
		std::lock_guard lock{inbox_mutex_};
		if (!accepting_ || inbox_.size() >= dns_pending_limit)
			return; /* the guest retries, as after a lost datagram */
		inbox_.push_back(
			{endpoints, {message.begin(), message.end()}});
	}
	wake();
}

void HostResolver::wake() noexcept
{
	const std::uint8_t value = 1;
	if (wake_pipe_[1].get() >= 0)
		(void)::write(wake_pipe_[1].get(), &value, sizeof(value));
}

int HostResolver::bind_listener(const std::string &path) noexcept
{
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	if (path.size() >= sizeof(address.sun_path))
		return -ENAMETOOLONG;
	std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
	UniqueDescriptor descriptor{::socket(AF_UNIX, SOCK_STREAM, 0)};
	if (descriptor.get() < 0 ||
	    !configureDescriptor(descriptor.get(),
				 {.closeOnExec = true, .nonblocking = true}))
		return -errno;
	/* The name carries this process's pid, so an existing entry is a
	 * crashed earlier run's of the same pid, never another live one. */
	(void)::unlink(path.c_str());
	if (::bind(descriptor.get(), reinterpret_cast<sockaddr *>(&address),
		   sizeof(address)) != 0)
		return -errno;
	try {
		listener_path_ = std::make_unique<uml::device::SocketPath>(path);
		listener_path_->adopt();
	} catch (...) {
		/* No free cleanup slot (or no memory): no stream path at all. */
		listener_path_.reset();
		(void)::unlink(path.c_str());
		return -ENOSPC;
	}
	(void)::chmod(path.c_str(), S_IRUSR | S_IWUSR);
	if (::listen(descriptor.get(), SOMAXCONN) != 0) {
		const int error = errno;
		listener_path_.reset();
		return -error;
	}
	listener_ = std::move(descriptor);
	return 0;
}

void HostResolver::run(std::stop_token stop_token) noexcept
{
	std::vector<pollfd> descriptors;
	std::vector<Stream *> polled;
	while (!stop_token.stop_requested()) {
		auto now = Clock::now();
		auto next = Clock::time_point::max();
		for (const auto &pending : pending_)
			next = std::min(next, pending->deadline);
		for (const auto &stream : streams_)
			if (stream.pending == 0)
				next = std::min(next, stream.active + dns_stream_idle);

		descriptors.clear();
		polled.clear();
		descriptors.push_back({wake_pipe_[0].get(), POLLIN, 0});
		const int resolver = connection_ != nullptr ?
			DNSServiceRefSockFD(connection_) : -1;
		descriptors.push_back({resolver, POLLIN, 0});
		descriptors.push_back({listener_.get(),
			static_cast<short>(streams_.size() < dns_stream_limit ?
				POLLIN : 0), 0});
		for (auto &stream : streams_) {
			short events = stream.input_closed ? 0 : POLLIN;
			if (!stream.output.empty())
				events |= POLLOUT;
			/* A half-closed stream with nothing to write waits for
			 * its answers; polling it would report its hang-up at
			 * once, every time. */
			descriptors.push_back(
				{events != 0 ? stream.descriptor.get() : -1,
				 events, 0});
			polled.push_back(&stream);
		}
		const int ready = ::poll(descriptors.data(),
			static_cast<nfds_t>(descriptors.size()),
			poll_timeout(now, next));
		if (stop_token.stop_requested())
			break;
		if (ready < 0 && errno != EINTR)
			break;

		if ((descriptors[0].revents & POLLIN) != 0) {
			std::array<std::uint8_t, 64> buffer{};
			while (::read(wake_pipe_[0].get(), buffer.data(),
				      buffer.size()) > 0)
				;
		}
		std::deque<Incoming> incoming;
		{
			std::lock_guard lock{inbox_mutex_};
			incoming.swap(inbox_);
		}
		for (const auto &query : incoming) {
			try {
				handle(query.endpoints, 0, query.message);
			} catch (...) {
				/* Out of memory: the guest retries. */
			}
		}

		if (resolver >= 0 &&
		    (descriptors[1].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
			if (DNSServiceProcessResult(connection_) !=
			    kDNSServiceErr_NoError)
				drop_connection();
			else if (batch_ended_)
				complete_ready();
		}
		if ((descriptors[2].revents & POLLIN) != 0)
			accept_streams();
		for (std::size_t index = 0; index < polled.size(); ++index) {
			const auto events = descriptors[3 + index].revents;
			if ((events & POLLOUT) != 0)
				flush_stream(*polled[index]);
			if ((events & (POLLIN | POLLERR | POLLHUP)) != 0)
				read_stream(*polled[index]);
		}
		expire(Clock::now());
	}
	/* Deallocating the shared connection ends every query it carries. */
	for (auto &pending : pending_)
		pending->reference = nullptr;
	pending_.clear();
	if (connection_ != nullptr) {
		DNSServiceRefDeallocate(connection_);
		connection_ = nullptr;
	}
	close_streams();
}

bool HostResolver::ensure_connection() noexcept
{
	if (connection_ != nullptr)
		return true;
	DNSServiceRef connection{};
	if (DNSServiceCreateConnection(&connection) != kDNSServiceErr_NoError)
		return false;
	connection_ = connection;
	return true;
}

void HostResolver::drop_connection() noexcept
{
	/* The daemon went away (mDNSResponder restarts, for one). Every query
	 * it held is lost; answer them and connect again on the next query. */
	for (auto &pending : pending_) {
		pending->reference = nullptr;
		pending->rcode = dns::rcode_servfail;
		pending->answers.clear();
		pending->expired = true;
	}
	DNSServiceRefDeallocate(connection_);
	connection_ = nullptr;
	complete_ready();
}

void HostResolver::handle(std::optional<dns::Endpoints> datagram,
			  std::uint64_t stream,
			  std::span<const std::uint8_t> message)
{
	auto parsed = dns::parse_query(message);
	if (parsed.disposition == dns::Disposition::drop)
		return;
	const std::size_t limit = datagram ?
		dns::udp_limit(parsed.query) : dns::tcp_limit;
	if (parsed.disposition == dns::Disposition::answer) {
		respond(datagram, stream,
			dns::build_response(parsed.query, parsed.rcode, {},
					    limit));
		return;
	}
	/* A stub that retransmits from the same socket asks once. */
	if (datagram) {
		const auto duplicate = std::ranges::any_of(
			pending_, [&](const auto &pending) {
				return pending->datagram &&
					pending->datagram->guest_port ==
						datagram->guest_port &&
					pending->datagram->server_address ==
						datagram->server_address &&
					pending->query.id == parsed.query.id &&
					pending->query.question ==
						parsed.query.question;
			});
		if (duplicate)
			return;
	}
	if (pending_.size() >= dns_pending_limit || !ensure_connection()) {
		respond(datagram, stream,
			dns::build_response(parsed.query, dns::rcode_servfail,
					    {}, limit));
		return;
	}
	auto pending = std::make_unique<Pending>();
	pending->owner = this;
	pending->datagram = datagram;
	pending->stream = stream;
	pending->query = std::move(parsed.query);
	pending->tail = pending->query.name;
	pending->deadline = Clock::now() + dns_query_deadline;
	const auto name = dns::presentation_name(pending->query.name);
	/* Listed before the query starts: once it has, its callback may name
	 * this entry, so nothing after that point may fail to keep it. */
	auto &entry = *pending_.emplace_back(std::move(pending));
	entry.reference = connection_;
	const auto error = DNSServiceQueryRecord(
		&entry.reference,
		kDNSServiceFlagsShareConnection |
			kDNSServiceFlagsReturnIntermediates,
		interface_index_, name.c_str(), entry.query.type,
		entry.query.klass, query_reply, &entry);
	if (error != kDNSServiceErr_NoError) {
		entry.reference = nullptr;
		const auto reply = dns::build_response(
			entry.query, dns::rcode_servfail, {}, limit);
		pending_.pop_back();
		respond(datagram, stream, reply);
		if (error == kDNSServiceErr_ServiceNotRunning ||
		    error == kDNSServiceErr_DefunctConnection)
			drop_connection();
		return;
	}
	if (auto *owner = find_stream(stream); owner != nullptr)
		++owner->pending;
}

void HostResolver::record(Pending &pending, std::uint32_t flags,
			  std::int32_t error, const char *fullname,
			  std::uint16_t type, std::uint16_t klass,
			  std::span<const std::uint8_t> data,
			  std::uint32_t ttl) noexcept
{
	/* MoreComing is collective on a shared connection: its absence ends
	 * the batch for every query (dns_sd.h, kDNSServiceFlagsShareConnection). */
	if ((flags & kDNSServiceFlagsMoreComing) == 0)
		batch_ended_ = true;
	if (pending.closed)
		return;
	const auto fail = [&pending]() {
		pending.rcode = dns::rcode_servfail;
		pending.answers.clear();
		pending.done = true;
		pending.closed = true;
	};
	switch (error) {
	case kDNSServiceErr_NoError:
		break;
	case kDNSServiceErr_NoSuchRecord:
		/* NODATA; the CNAMEs that led here stand. A negative answer
		 * after a positive one (a cache change) is ignored. */
		if (!pending.done)
			pending.done = pending.closed = true;
		return;
	case kDNSServiceErr_NoSuchName:
		if (!pending.done) {
			pending.rcode = dns::rcode_nxdomain;
			pending.done = pending.closed = true;
		}
		return;
	default:
		fail();
		return;
	}
	if ((flags & kDNSServiceFlagsAdd) == 0 || fullname == nullptr)
		return;
	try {
		auto owner = dns::wire_name(fullname);
		if (!owner || !dns::same_name(*owner, pending.tail))
			return;
		const auto wanted = pending.query.type;
		if (type == wanted || wanted == dns::type_any) {
			pending.answers.push_back(
				{std::move(*owner), type, klass, ttl,
				 {data.begin(), data.end()}});
			pending.done = true;
		} else if (type == dns::type_cname && !pending.done) {
			auto target = dns::cname_target(data);
			if (!target)
				return;
			pending.answers.push_back(
				{std::move(*owner), type, klass, ttl,
				 {data.begin(), data.end()}});
			pending.tail = std::move(*target);
			if (++pending.aliases > alias_limit)
				fail();
		}
	} catch (...) {
		fail();
	}
}

void HostResolver::complete_ready() noexcept
{
	const bool batch_ended = std::exchange(batch_ended_, false);
	for (auto iterator = pending_.begin(); iterator != pending_.end();) {
		const auto &pending = **iterator;
		if (!pending.expired && !(pending.done && batch_ended)) {
			++iterator;
			continue;
		}
		finish(**iterator);
		iterator = pending_.erase(iterator);
	}
}

void HostResolver::finish(Pending &pending) noexcept
{
	if (pending.reference != nullptr) {
		DNSServiceRefDeallocate(pending.reference);
		pending.reference = nullptr;
	}
	if (auto *stream = find_stream(pending.stream);
	    stream != nullptr && stream->pending != 0)
		--stream->pending;
	try {
		const std::size_t limit = pending.datagram ?
			dns::udp_limit(pending.query) : dns::tcp_limit;
		respond(pending.datagram, pending.stream,
			dns::build_response(pending.query, pending.rcode,
					    pending.answers, limit));
	} catch (...) {
	}
#if UML_SLIRP_TESTING
	resolved_for_testing_.fetch_add(1, std::memory_order_relaxed);
#endif
}

void HostResolver::expire(Clock::time_point now) noexcept
{
	bool any = false;
	for (auto &pending : pending_)
		if (pending->deadline <= now) {
			pending->rcode = dns::rcode_servfail;
			pending->answers.clear();
			pending->expired = true;
			any = true;
		}
	if (any) {
		/* Keep a batch end that has not been acted on. */
		const bool batch_ended = batch_ended_;
		batch_ended_ = false;
		complete_ready();
		batch_ended_ = batch_ended;
	}
	for (auto iterator = streams_.begin(); iterator != streams_.end();) {
		/* A stream that made no progress for the idle time goes, even
		 * with output a reader never takes: poll() would otherwise
		 * return at once for its deadline, again and again. */
		if (iterator->descriptor.get() < 0 ||
		    (iterator->pending == 0 &&
		     ((iterator->input_closed && iterator->output.empty()) ||
		      iterator->active + dns_stream_idle <= now)))
			iterator = streams_.erase(iterator);
		else
			++iterator;
	}
}

void HostResolver::respond(const std::optional<dns::Endpoints> &datagram,
			   std::uint64_t stream,
			   std::span<const std::uint8_t> reply)
{
	if (datagram) {
		if (sink_)
			sink_(dns::udp_reply_frame(*datagram, reply,
						   next_identification_++));
		return;
	}
	auto *owner = find_stream(stream);
	if (owner == nullptr || reply.size() > dns::tcp_limit)
		return;
	if (owner->output.size() + 2 + reply.size() > stream_buffer_limit) {
		owner->descriptor.reset(); /* a guest that never reads */
		return;
	}
	owner->output.push_back(static_cast<std::uint8_t>(reply.size() >> 8));
	owner->output.push_back(static_cast<std::uint8_t>(reply.size()));
	owner->output.insert(owner->output.end(), reply.begin(), reply.end());
	flush_stream(*owner);
}

void HostResolver::accept_streams() noexcept
{
	while (streams_.size() < dns_stream_limit) {
		UniqueDescriptor descriptor{::accept(listener_.get(), nullptr,
						     nullptr)};
		if (descriptor.get() < 0)
			return;
		/* Only SLiRP, in this process, may use the resolver. */
		pid_t peer = -1;
		socklen_t length = sizeof(peer);
		if (::getsockopt(descriptor.get(), SOL_LOCAL, LOCAL_PEERPID,
				 &peer, &length) != 0 || peer != ::getpid())
			continue;
		if (!configureDescriptor(descriptor.get(),
					 {.closeOnExec = true,
					  .nonblocking = true,
					  .noSigpipe = true}))
			continue;
		try {
			auto &stream = streams_.emplace_back();
			stream.serial = next_stream_++;
			stream.descriptor = std::move(descriptor);
			stream.active = Clock::now();
		} catch (...) {
			return;
		}
	}
}

void HostResolver::read_stream(Stream &stream) noexcept
{
	if (stream.descriptor.get() < 0)
		return;
	std::array<std::uint8_t, 4096> buffer{};
	for (;;) {
		const auto count = ::read(stream.descriptor.get(), buffer.data(),
					  buffer.size());
		if (count == 0) {
			/* The client closed its sending side; it may still be
			 * reading the answers to what it sent. */
			stream.input_closed = true;
			break;
		}
		if (count < 0 && errno != EAGAIN && errno != EINTR) {
			stream.descriptor.reset();
			break;
		}
		if (count < 0)
			break;
		if (stream.input.size() + static_cast<std::size_t>(count) >
		    stream_buffer_limit) {
			stream.descriptor.reset();
			break;
		}
		try {
			stream.input.insert(stream.input.end(), buffer.begin(),
					    buffer.begin() + count);
		} catch (...) {
			stream.descriptor.reset();
			break;
		}
	}
	stream.active = Clock::now();
	const auto serial = stream.serial;
	std::size_t offset = 0;
	while (stream.input.size() - offset >= 2) {
		const std::size_t length = (stream.input[offset] << 8) |
			stream.input[offset + 1];
		if (stream.input.size() - offset - 2 < length)
			break;
		try {
			const std::vector<std::uint8_t> message(
				stream.input.begin() + offset + 2,
				stream.input.begin() + offset + 2 + length);
			handle(std::nullopt, serial, message);
		} catch (...) {
		}
		offset += 2 + length;
	}
	/* A closed stream leaves at the next expire(); queries still in
	 * flight for it are answered into nothing. */
	stream.input.erase(stream.input.begin(), stream.input.begin() + offset);
}

void HostResolver::flush_stream(Stream &stream) noexcept
{
	while (!stream.output.empty() && stream.descriptor.get() >= 0) {
		const auto count = ::write(stream.descriptor.get(),
					   stream.output.data(),
					   stream.output.size());
		if (count < 0) {
			if (errno != EAGAIN && errno != EINTR)
				stream.descriptor.reset();
			return;
		}
		stream.output.erase(stream.output.begin(),
				    stream.output.begin() + count);
		stream.active = Clock::now();
	}
}

void HostResolver::close_streams() noexcept
{
	streams_.clear();
}

HostResolver::Stream *HostResolver::find_stream(std::uint64_t serial) noexcept
{
	if (serial == 0)
		return nullptr;
	for (auto &stream : streams_)
		if (stream.serial == serial)
			return stream.descriptor.get() >= 0 ? &stream : nullptr;
	return nullptr;
}

#if UML_SLIRP_TESTING
std::uint64_t HostResolver::resolved_for_testing() const noexcept
{
	return resolved_for_testing_.load(std::memory_order_relaxed);
}
#endif

} // namespace uml::network
