// SPDX-License-Identifier: GPL-2.0-or-later
#include "session_record.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mach/mach.h>
#include <os/lock.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <time.h>
#include <unistd.h>

namespace uml::runtime {
namespace {

/* The kernel output a record keeps: enough for a panic's message, its stack
 * and the lines before it. */
constexpr std::size_t output_size = 4096;
constexpr std::size_t console_slots = 4;
/* Darwin's limit on a thread name, its terminator included. */
constexpr std::size_t thread_name_size = 64;
constexpr std::string_view panic_prefix{"Darwin UML panic: "};
/* What the kernel's handler (os-Darwin/signal.c) writes, one write each, when
 * a host thread that owns no vCPU faults: the registers, then this. */
constexpr std::string_view host_fault_prefix{"Darwin host fault: "};
constexpr std::string_view host_fault_registers{"Darwin host fault: signal="};
constexpr std::string_view host_fault_line{
	"Darwin host fault: taken on a host thread which owns no virtual CPU"};

std::atomic<char> output[output_size];
std::atomic<std::uint64_t> output_end{0};
std::atomic<int> console_descriptors[console_slots] = {-1, -1, -1, -1};

std::atomic<int> record_descriptor{-1};
/* True while no end has been recorded for the session that began last. */
std::atomic<bool> session_open{false};
std::atomic<std::int64_t> session_started{0};
std::atomic<bool> exit_hook_installed{false};
std::atomic<bool> crash_recorded{false};

/* The record and the output it quotes: static, so an end formats nothing on
 * a stack it does not know the size of. One end writes them, the first. */
char record_text[output_size + 1024];
char tail_text[output_size];

/* The record's first bytes are a fixed slot that only the checkpoints write
 * and the rest is the session's state that only its begin and its end write,
 * so neither ever overwrites the other's bytes, whichever finishes last. */
constexpr std::size_t memory_slot_size = session_memory_slot_bytes;
std::atomic<bool> checkpoint_thread_started{false};

std::atomic<MemoryTermsReader> terms_reader{nullptr};

/* One checkpoint's figures in MiB, as the history keeps them. */
struct HistoryEntry final {
	std::int64_t seconds;
	std::uint64_t footprint;
	std::uint64_t available;
	std::uint64_t guest;
	std::uint64_t gpu;
	std::uint64_t graphics;
	std::uint64_t cache;
	std::uint64_t heaps;
	std::uint64_t malloc;
	std::uint64_t other;
};

/* Under history_lock: the checkpoints of the session that began last. The
 * thread is the only writer but for tests, which call the checkpoint
 * directly. */
os_unfair_lock history_lock = OS_UNFAIR_LOCK_INIT;
HistoryEntry recent[memory_history_recent];
HistoryEntry older[memory_history_older];
std::uint64_t checkpoints_taken{};
std::size_t older_kept{};
/* The highest footprint a checkpoint read, and the terms beside it. */
std::uint64_t peak_footprint{};
std::int64_t peak_at{};
MemoryTerms peak_terms{};
std::int64_t peak_malloc_age_ms{-1};
/* The last reading of malloc's term (memory_malloc_reading_due): when, on
 * the steady clock (negative for none), and the highest footprint any
 * reading was taken at. */
std::int64_t malloc_read_ms{-1};
std::uint64_t malloc_read_peak{};
std::uint64_t malloc_allocated{};
std::uint64_t malloc_in_use{};

bool kept(int descriptor) noexcept
{
	if (descriptor == STDOUT_FILENO || descriptor == STDERR_FILENO)
		return true;
	for (const auto &console : console_descriptors)
		if (console.load(std::memory_order_relaxed) == descriptor)
			return true;
	return false;
}

void keep_output(const void *data, std::size_t length) noexcept
{
	const auto *bytes = static_cast<const char *>(data);
	if (length > output_size) {
		bytes += length - output_size;
		length = output_size;
	}
	const std::uint64_t start =
		output_end.fetch_add(length, std::memory_order_acq_rel);
	for (std::size_t index = 0; index < length; ++index)
		output[(start + index) % output_size].store(bytes[index],
			std::memory_order_relaxed);
}

/* Formats into a fixed buffer; what does not fit is dropped. */
class Text final {
public:
	Text(char *data, std::size_t size) noexcept : data_{data}, size_{size} {}

	Text &text(std::string_view value) noexcept
	{
		const std::size_t count = std::min(value.size(), size_ - used_);
		std::memcpy(data_ + used_, value.data(), count);
		used_ += count;
		return *this;
	}

	Text &number(std::int64_t value) noexcept
	{
		char digits[24];
		const auto result =
			std::to_chars(digits, digits + sizeof(digits), value);
		return text({digits, static_cast<std::size_t>(result.ptr - digits)});
	}

	/* `value` with every byte that is not printable ASCII, or a line feed
	 * where `lines` allows one, replaced; carriage returns are dropped. */
	Text &printable(std::string_view value, bool lines) noexcept
	{
		for (const char character : value) {
			if (used_ == size_)
				break;
			if (character == '\r')
				continue;
			const bool keep = (character >= ' ' && character < 0x7f) ||
				(lines && (character == '\n' || character == '\t'));
			data_[used_++] = keep ? character : '?';
		}
		return *this;
	}

	[[nodiscard]] std::size_t size() const noexcept
	{
		return used_;
	}

private:
	char *data_;
	std::size_t size_;
	std::size_t used_{};
};

std::int64_t now() noexcept
{
	timespec time{};
	(void)clock_gettime(CLOCK_REALTIME, &time);
	return static_cast<std::int64_t>(time.tv_sec);
}

constexpr std::uint64_t mebibytes(std::uint64_t bytes) noexcept
{
	return bytes >> 20;
}

Text &history_line(Text &text, const HistoryEntry &entry) noexcept
{
	return text.text("h ").number(entry.seconds)
		.text(" ").number(static_cast<std::int64_t>(entry.footprint))
		.text(" ").number(static_cast<std::int64_t>(entry.available))
		.text(" ").number(static_cast<std::int64_t>(entry.guest))
		.text(" ").number(static_cast<std::int64_t>(entry.gpu))
		.text(" ").number(static_cast<std::int64_t>(entry.graphics))
		.text(" ").number(static_cast<std::int64_t>(entry.cache))
		.text(" ").number(static_cast<std::int64_t>(entry.heaps))
		.text(" ").number(static_cast<std::int64_t>(entry.malloc))
		.text(" ").number(static_cast<std::int64_t>(entry.other))
		.text("\n");
}

Text &mib(Text &text, std::string_view name, std::uint64_t bytes) noexcept
{
	return text.text(" ").text(name).text(" ")
		.number(static_cast<std::int64_t>(mebibytes(bytes)));
}

std::int64_t steady_ms() noexcept
{
	return static_cast<std::int64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
			.count());
}

/* The footprint less the terms read at every checkpoint: malloc and what no
 * term names. */
std::uint64_t other_bytes(std::uint64_t footprint,
	const MemoryTerms &terms) noexcept
{
	const std::uint64_t named = terms.guest_held + terms.graphics +
		terms.media + terms.cache + terms.heaps;
	return footprint > named ? footprint - named : 0;
}

/* "guest G gpu H/B [age S] ... malloc A/U [age S] other R ... compressed K",
 * in MiB and seconds. */
Text &terms_words(Text &text, const MemoryTerms &terms,
	std::int64_t malloc_age_ms, std::uint64_t footprint) noexcept
{
	mib(text, "guest", terms.guest_held);
	if (terms.gpu_budget != 0) {
		mib(text, "gpu", terms.gpu_held).text("/").number(
			static_cast<std::int64_t>(mebibytes(terms.gpu_budget)));
		if (terms.gpu_age_ms >= 0)
			text.text(" age ").number(terms.gpu_age_ms / 1000);
	}
	mib(text, "graphics", terms.graphics);
	mib(text, "media", terms.media);
	mib(text, "cache", terms.cache);
	mib(text, "heaps", terms.heaps);
	mib(text, "malloc", terms.malloc_allocated).text("/").number(
		static_cast<std::int64_t>(mebibytes(terms.malloc_in_use)));
	if (malloc_age_ms >= 0)
		text.text(" age ").number(malloc_age_ms / 1000);
	mib(text, "other", other_bytes(footprint, terms));
	mib(text, "internal", terms.internal);
	return mib(text, "compressed", terms.compressed);
}

void forget_history() noexcept
{
	os_unfair_lock_lock(&history_lock);
	checkpoints_taken = 0;
	older_kept = 0;
	peak_footprint = 0;
	peak_at = 0;
	peak_terms = {};
	peak_malloc_age_ms = -1;
	malloc_read_ms = -1;
	malloc_read_peak = 0;
	malloc_allocated = 0;
	malloc_in_use = 0;
	os_unfair_lock_unlock(&history_lock);
}

/* The memory slot: the format line, then the process's footprint, its peak
 * and what its limit leaves, in bytes; with a terms reader, the terms now and
 * at the highest checkpoint and the history, in MiB; padded to the slot's
 * size. */
void write_memory_slot(int descriptor) noexcept
{
	static char slot[memory_slot_size];
	os_unfair_lock_lock(&history_lock);
	Text text{slot, sizeof(slot) - 1};
	const std::int64_t at = now();
	text.text("uml-session 3\nmemory at ").number(at);
	task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	std::uint64_t footprint = 0;
	std::uint64_t available = 0;
	if (task_info(mach_task_self(), TASK_VM_INFO,
		    reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
		footprint = info.phys_footprint;
		text.text(" footprint ")
			.number(static_cast<std::int64_t>(info.phys_footprint))
			.text(" peak ")
			.number(static_cast<std::int64_t>(
				info.ledger_phys_footprint_peak));
		/* Zero where the host sets no limit, as on a Mac. */
		if (count >= TASK_VM_INFO_COUNT && info.limit_bytes_remaining != 0) {
			available = info.limit_bytes_remaining;
			text.text(" available ").number(static_cast<std::int64_t>(
				info.limit_bytes_remaining));
		}
	}
	text.text("\n");
	if (const auto reader = terms_reader.load(std::memory_order_acquire)) {
		const std::int64_t clock = steady_ms();
		const bool read_malloc = memory_malloc_reading_due(
			malloc_read_ms >= 0, footprint, malloc_read_peak);
		MemoryTerms terms = reader(read_malloc);
		if (read_malloc) {
			malloc_read_ms = clock;
			malloc_read_peak = std::max(malloc_read_peak, footprint);
			malloc_allocated = terms.malloc_allocated;
			malloc_in_use = terms.malloc_in_use;
		} else {
			terms.malloc_allocated = malloc_allocated;
			terms.malloc_in_use = malloc_in_use;
		}
		const std::int64_t malloc_age = clock - malloc_read_ms;
		const HistoryEntry entry{
			.seconds = at - session_started.load(std::memory_order_relaxed),
			.footprint = mebibytes(footprint),
			.available = mebibytes(available),
			.guest = mebibytes(terms.guest_held),
			.gpu = mebibytes(terms.gpu_held),
			.graphics = mebibytes(terms.graphics),
			.cache = mebibytes(terms.cache),
			.heaps = mebibytes(terms.heaps),
			.malloc = mebibytes(terms.malloc_allocated),
			.other = mebibytes(other_bytes(footprint, terms)),
		};
		/* A recent entry about to be overwritten moves to the older
		 * history when its turn in the stride comes. */
		const std::uint64_t index = checkpoints_taken++;
		if (index >= memory_history_recent) {
			const std::uint64_t leaving = index - memory_history_recent;
			if (leaving % memory_history_older_stride == 0)
				older[older_kept++ % memory_history_older] =
					recent[leaving % memory_history_recent];
		}
		recent[index % memory_history_recent] = entry;
		if (footprint >= peak_footprint) {
			peak_footprint = footprint;
			peak_at = at;
			peak_terms = terms;
			peak_malloc_age_ms = malloc_age;
		}
		terms_words(text.text("terms"), terms, malloc_age, footprint)
			.text("\n");
		text.text("peak-terms at ").number(peak_at).text(" footprint ")
			.number(static_cast<std::int64_t>(mebibytes(peak_footprint)));
		terms_words(text, peak_terms, peak_malloc_age_ms, peak_footprint)
			.text("\n");
		text.text("history seconds footprint available guest gpu graphics "
			"cache heaps malloc other\n");
		const std::size_t olders = std::min<std::size_t>(older_kept,
			memory_history_older);
		for (std::size_t n = 0; n < olders; ++n)
			history_line(text, older[(older_kept - olders + n) %
				memory_history_older]);
		const std::uint64_t recents = std::min<std::uint64_t>(
			checkpoints_taken, memory_history_recent);
		for (std::uint64_t n = 0; n < recents; ++n)
			history_line(text, recent[(checkpoints_taken - recents + n) %
				memory_history_recent]);
	}
	const std::size_t used = text.size();
	std::memset(slot + used, ' ', sizeof(slot) - 1 - used);
	slot[sizeof(slot) - 1] = '\n';
	ssize_t written;
	do {
		written = pwrite(descriptor, slot, sizeof(slot), 0);
	} while (written < 0 && errno == EINTR);
	os_unfair_lock_unlock(&history_lock);
}

void *checkpoint_memory(void *) noexcept
{
	(void)pthread_setname_np("uml-session-record");
	for (;;) {
		timespec interval{
			.tv_sec = static_cast<time_t>(memory_checkpoint_interval_ms /
				1000),
			.tv_nsec = static_cast<long>(
				(memory_checkpoint_interval_ms % 1000) * 1000000),
		};
		while (nanosleep(&interval, &interval) != 0 && errno == EINTR) {
		}
		if (session_open.load(std::memory_order_acquire))
			checkpoint_session_memory();
	}
	return nullptr;
}

/* One thread for the process's life: it never touches a session that has
 * ended, and blocks every signal, so the kernel's process-directed signals
 * never pick it. */
void start_checkpoints() noexcept
{
	if (checkpoint_thread_started.exchange(true))
		return;
	sigset_t all;
	sigset_t previous;
	sigfillset(&all);
	if (pthread_sigmask(SIG_SETMASK, &all, &previous) != 0)
		return;
	pthread_attr_t attributes;
	pthread_t thread;
	if (pthread_attr_init(&attributes) == 0) {
		(void)pthread_attr_setdetachstate(&attributes,
			PTHREAD_CREATE_DETACHED);
		(void)pthread_attr_setstacksize(&attributes, 64 * 1024);
		if (pthread_create(&thread, &attributes, checkpoint_memory,
			    nullptr) != 0)
			checkpoint_thread_started.store(false);
		(void)pthread_attr_destroy(&attributes);
	}
	(void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}

void write_record(SessionEnd end, int status, std::string_view message,
	std::string_view tail) noexcept
{
	const int descriptor = record_descriptor.load(std::memory_order_acquire);
	if (descriptor < 0)
		return;
	Text record{record_text, sizeof(record_text)};
	record.text("state ")
		.text(end == SessionEnd::running ? "running" : "ended")
		.text("\nend ").text(session_end_name(end))
		.text("\nstatus ").number(status)
		.text("\npid ").number(getpid())
		.text("\nstarted ")
		.number(session_started.load(std::memory_order_relaxed));
	if (end != SessionEnd::running)
		record.text("\nended ").number(now());
	if (!message.empty())
		record.text("\nmessage ").printable(message, false);
	record.text("\n");
	if (!tail.empty())
		record.text("kernel-output\n").printable(tail, true);
	ssize_t written;
	do {
		written = pwrite(descriptor, record_text, record.size(),
			memory_slot_size);
	} while (written < 0 && errno == EINTR);
	if (written == static_cast<ssize_t>(record.size()))
		(void)ftruncate(descriptor,
			static_cast<off_t>(memory_slot_size + record.size()));
}

std::string_view output_tail() noexcept
{
	return {tail_text, kernel_output_tail(tail_text)};
}

/* Records the session's first end; `quote_output` adds the kernel's last
 * output, which only the first end reads into the static copy. */
void end_session(SessionEnd end, int status, std::string_view message,
	bool quote_output) noexcept
{
	if (!session_open.exchange(false, std::memory_order_acq_rel))
		return;
	write_record(end, status, message,
		quote_output ? output_tail() : std::string_view{});
}

void record_process_exit()
{
	end_session(SessionEnd::process_exit, 0,
		"exit() was called while the guest ran", true);
}

/* The signal number in a host-fault report's registers, else 0. */
int fault_signal(std::string_view registers) noexcept
{
	constexpr std::string_view key{"signal="};
	if (!registers.starts_with(key))
		return 0;
	int signal_number = 0;
	(void)std::from_chars(registers.data() + key.size(),
		registers.data() + registers.size(), signal_number);
	return signal_number;
}

void record_host_fault() noexcept
{
	if (crash_recorded.exchange(true, std::memory_order_acq_rel))
		return;
	if (!session_open.exchange(false, std::memory_order_acq_rel))
		return;
	const auto tail = output_tail();
	const auto registers = host_fault_message(tail);
	write_record(SessionEnd::host_fault, fault_signal(registers), registers,
		tail);
}

void name_thread(std::string_view message) noexcept
{
	char name[thread_name_size];
	Text text{name, sizeof(name) - 1};
	text.text("panic: ").printable(message, false);
	name[text.size()] = '\0';
	(void)pthread_setname_np(name);
}

} // namespace

std::string_view session_end_name(SessionEnd end) noexcept
{
	switch (end) {
	case SessionEnd::running:
		return "running";
	case SessionEnd::powered_off:
		return "powered-off";
	case SessionEnd::restarted:
		return "restarted";
	case SessionEnd::stopped:
		return "stopped";
	case SessionEnd::failed:
		return "failed";
	case SessionEnd::guest_panic:
		return "guest-panic";
	case SessionEnd::kernel_abort:
		return "kernel-abort";
	case SessionEnd::kernel_exit:
		return "kernel-exit";
	case SessionEnd::hibernation_restore_failed:
		return "hibernation-restore-failed";
	case SessionEnd::process_exit:
		return "process-exit";
	case SessionEnd::host_fault:
		return "host-fault";
	}
	return "unknown";
}

std::size_t console_output_descriptors(
	std::span<const std::string> kernel_arguments, std::span<int> descriptors)
	noexcept
{
	std::size_t found = 0;
	for (const auto &argument : kernel_arguments) {
		const std::string_view text{argument};
		const auto equals = text.find('=');
		if (equals == std::string_view::npos)
			continue;
		const auto name = text.substr(0, equals);
		const auto channel = name.starts_with("con") ? name.substr(3) :
			name.starts_with("ssl") ? name.substr(3) :
			std::string_view{"-"};
		if (!std::all_of(channel.begin(), channel.end(),
			    [](char c) { return c >= '0' && c <= '9'; }))
			continue;
		/* fd:IN,fd:OUT writes to OUT; fd:BOTH to BOTH. */
		auto value = text.substr(equals + 1);
		if (const auto comma = value.find(','); comma != value.npos)
			value = value.substr(comma + 1);
		if (!value.starts_with("fd:"))
			continue;
		value.remove_prefix(3);
		int descriptor = -1;
		const auto parsed = std::from_chars(value.data(),
			value.data() + value.size(), descriptor);
		if (parsed.ec != std::errc{} ||
		    parsed.ptr != value.data() + value.size() || descriptor < 0)
			continue;
		if (found < descriptors.size())
			descriptors[found] = descriptor;
		++found;
	}
	return std::min(found, descriptors.size());
}

bool begin_session_record(std::string_view path,
	std::span<const std::string> kernel_arguments) noexcept
{
	int consoles[console_slots] = {-1, -1, -1, -1};
	(void)console_output_descriptors(kernel_arguments, consoles);
	for (std::size_t slot = 0; slot < console_slots; ++slot)
		console_descriptors[slot].store(consoles[slot],
			std::memory_order_relaxed);
	if (path.empty())
		return true;
	const std::string name{path};
	int descriptor;
	do {
		descriptor = open(name.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC,
			0600);
	} while (descriptor < 0 && errno == EINTR);
	if (descriptor < 0) {
		const int error = errno;
		dprintf(STDERR_FILENO,
			"uml: the session record cannot be opened: %s\n",
			strerror(error));
		return false;
	}
	/* The record keeps one descriptor number for the process's life: a
	 * checkpoint may hold it at any time, and a closed number another open
	 * reused would take the checkpoint's bytes. A later session moves its
	 * file onto that number. */
	if (const int kept_descriptor =
		    record_descriptor.load(std::memory_order_acquire);
	    kept_descriptor >= 0) {
		const int moved = dup2(descriptor, kept_descriptor);
		(void)close(descriptor);
		if (moved < 0)
			return false;
		descriptor = kept_descriptor;
		(void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);
	} else {
		record_descriptor.store(descriptor, std::memory_order_release);
	}
	if (!exit_hook_installed.exchange(true))
		(void)atexit(record_process_exit);
	session_started.store(now(), std::memory_order_relaxed);
	forget_history();
	write_memory_slot(descriptor);
	session_open.store(true, std::memory_order_release);
	write_record(SessionEnd::running, 0, {}, {});
	start_checkpoints();
	return true;
}

void set_memory_terms_reader(MemoryTermsReader reader) noexcept
{
	terms_reader.store(reader, std::memory_order_release);
}

void checkpoint_session_memory() noexcept
{
	const int descriptor = record_descriptor.load(std::memory_order_acquire);
	if (descriptor >= 0)
		write_memory_slot(descriptor);
}

void end_session_record(SessionEnd end, int status,
	std::string_view message) noexcept
{
	end_session(end, status, message, end != SessionEnd::powered_off &&
		end != SessionEnd::restarted && end != SessionEnd::stopped);
}

void record_kernel_crash() noexcept
{
	/* A second vCPU that crashes while the first records leaves the first
	 * one's record and name alone. */
	if (crash_recorded.exchange(true, std::memory_order_acq_rel))
		return;
	const auto tail = output_tail();
	const auto message = panic_message(tail);
	if (!message.empty())
		name_thread(message);
	if (!session_open.exchange(false, std::memory_order_acq_rel))
		return;
	write_record(message.empty() ? SessionEnd::kernel_abort :
		SessionEnd::guest_panic, SIGABRT, message, tail);
}

ssize_t kernel_write(int descriptor, const void *buffer, std::size_t length)
	noexcept
{
	if (length != 0 && buffer != nullptr && kept(descriptor)) {
		keep_output(buffer, length);
		/* The fault ends the process as soon as the handler returns. */
		if (descriptor == STDERR_FILENO &&
		    std::string_view{static_cast<const char *>(buffer), length}
			    .starts_with(host_fault_line))
			record_host_fault();
	}
	return write(descriptor, buffer, length);
}

void *kernel_output_import(const char *name) noexcept
{
	if (name != nullptr && name[0] == '_')
		++name;
	return name != nullptr && std::strcmp(name, "write") == 0 ?
		reinterpret_cast<void *>(&kernel_write) : nullptr;
}

std::size_t kernel_output_tail(std::span<char> buffer) noexcept
{
	const std::uint64_t end = output_end.load(std::memory_order_acquire);
	const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(
		{end, static_cast<std::uint64_t>(output_size),
			static_cast<std::uint64_t>(buffer.size())}));
	for (std::size_t index = 0; index < count; ++index)
		buffer[index] = output[(end - count + index) % output_size].load(
			std::memory_order_relaxed);
	return count;
}

std::string_view panic_message(std::string_view output_text) noexcept
{
	const auto start = output_text.rfind(panic_prefix);
	if (start == std::string_view::npos)
		return {};
	auto message = output_text.substr(start + panic_prefix.size());
	if (const auto end = message.find('\n'); end != message.npos)
		message = message.substr(0, end);
	return message;
}

std::string_view host_fault_message(std::string_view output_text) noexcept
{
	const auto start = output_text.rfind(host_fault_registers);
	if (start == std::string_view::npos)
		return {};
	auto message = output_text.substr(start + host_fault_prefix.size());
	if (const auto end = message.find('\n'); end != message.npos)
		message = message.substr(0, end);
	return message;
}

void reset_session_record() noexcept
{
	session_open.store(false, std::memory_order_release);
	/* The number stays the record's (begin_session_record), on no file. */
	if (const int descriptor = record_descriptor.load(); descriptor >= 0) {
		if (const int empty = open("/dev/null", O_WRONLY | O_CLOEXEC);
		    empty >= 0) {
			(void)dup2(empty, descriptor);
			(void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);
			(void)close(empty);
		}
	}
	for (auto &console : console_descriptors)
		console.store(-1, std::memory_order_relaxed);
	output_end.store(0, std::memory_order_release);
	crash_recorded.store(false, std::memory_order_release);
}

} // namespace uml::runtime
