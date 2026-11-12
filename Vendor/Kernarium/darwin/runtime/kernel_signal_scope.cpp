// SPDX-License-Identifier: GPL-2.0-or-later
#include "kernel_signal_scope.hpp"
#include "memory/shared_backing.hpp"

#include <atomic>
#include <cstring>
#include <crt_externs.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

namespace uml::runtime {
namespace {

[[nodiscard]] bool revocable(int signal) noexcept
{
	return signal != SIGKILL && signal != SIGSTOP;
}

[[nodiscard]] std::uintptr_t handler_address(
	const struct sigaction &action) noexcept
{
	if ((action.sa_flags & SA_SIGINFO) != 0)
		return reinterpret_cast<std::uintptr_t>(action.sa_sigaction);
	return reinterpret_cast<std::uintptr_t>(action.sa_handler);
}

/* The status flags F_SETFL can change on an open file description. */
constexpr int settable_status_flags =
	O_NONBLOCK | O_APPEND | O_ASYNC | O_FSYNC | O_DSYNC;

[[nodiscard]] bool same_disposition(const struct sigaction &left,
	const struct sigaction &right) noexcept
{
	return left.sa_handler == right.sa_handler &&
		left.sa_flags == right.sa_flags &&
		left.sa_mask == right.sa_mask;
}

/* PENDIN is the line discipline's own note that input is waiting to be
 * retyped: a terminal sets it after any change of mode, and the mode it
 * records does not include it. */
[[nodiscard]] bool same_terminal(const struct termios &left,
	const struct termios &right) noexcept
{
	return left.c_iflag == right.c_iflag && left.c_oflag == right.c_oflag &&
		left.c_cflag == right.c_cflag &&
		(left.c_lflag & ~PENDIN) == (right.c_lflag & ~PENDIN) &&
		std::memcmp(left.c_cc, right.c_cc, sizeof(left.c_cc)) == 0 &&
		left.c_ispeed == right.c_ispeed && left.c_ospeed == right.c_ospeed;
}

[[nodiscard]] bool same_stack(const stack_t &left,
	const stack_t &right) noexcept
{
	if (((left.ss_flags ^ right.ss_flags) & SS_DISABLE) != 0)
		return false;
	return (left.ss_flags & SS_DISABLE) != 0 ||
		(left.ss_sp == right.ss_sp && left.ss_size == right.ss_size);
}

/* Changes a terminal from a process that may be in a background group of it
 * without being stopped by SIGTTOU: POSIX lets a process that blocks the
 * signal change the terminal. Not draining output, which a terminal nobody
 * reads would never finish. */
void restore_terminal(int descriptor, const struct termios &attributes) noexcept
{
	sigset_t stop;
	sigemptyset(&stop);
	sigaddset(&stop, SIGTTOU);
	sigset_t previous;
	const bool blocked = pthread_sigmask(SIG_BLOCK, &stop, &previous) == 0;
	(void)tcsetattr(descriptor, TCSANOW, &attributes);
	if (blocked)
		(void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}

} // namespace

KernelSignalScope::KernelSignalScope(std::uintptr_t image_begin,
	std::uintptr_t image_end) noexcept
	: image_begin_{image_begin}, image_end_{image_end}
{
	for (int signal = 1; signal < NSIG; ++signal)
		recorded_[signal] = revocable(signal) &&
			sigaction(signal, nullptr, &dispositions_[signal]) == 0;
	mask_recorded_ = pthread_sigmask(SIG_SETMASK, nullptr, &mask_) == 0;
	alternate_stack_recorded_ = sigaltstack(nullptr, &alternate_stack_) == 0;
	for (std::size_t descriptor = 0; descriptor < standard_flags_.size();
	     ++descriptor) {
		const int fd = static_cast<int>(descriptor);
		const int flags = fcntl(fd, F_GETFL);
		if (flags < 0)
			continue;
		standard_flags_[descriptor] = flags & settable_status_flags;
		standard_terminal_recorded_[descriptor] = isatty(fd) == 1 &&
			tcgetattr(fd, &standard_terminals_[descriptor]) == 0;
	}
}

KernelSignalScope::~KernelSignalScope()
{
	/* Stop the kernel's only self-directed signal source first, so no tick
	 * can be raised after its pending instances have been discarded. */
	const itimerval disarmed{};
	(void)setitimer(ITIMER_REAL, &disarmed, nullptr);

	for (int signal = 1; signal < NSIG; ++signal) {
		struct sigaction current {};
		if (!revocable(signal) ||
		    sigaction(signal, nullptr, &current) != 0)
			continue;
		const bool kernel_code = inside_image(current);
		if (!kernel_code) {
			/* A disposition of the kernel's own making, such as the SIG_IGN
			 * it leaves SIGPIPE, SIGIO and SIGWINCH with, references no
			 * kernel code, but the next boot must find what this one did. */
			if (recorded_[signal] &&
			    !same_disposition(current, dispositions_[signal]))
				(void)sigaction(signal, &dispositions_[signal], nullptr);
			continue;
		}
		/* Ignoring a signal discards every pending instance of it, including
		 * one Darwin parked on a thread which still blocks it. */
		struct sigaction ignore {};
		ignore.sa_handler = SIG_IGN;
		sigemptyset(&ignore.sa_mask);
		(void)sigaction(signal, &ignore, nullptr);

		struct sigaction restored {};
		restored.sa_handler = SIG_DFL;
		sigemptyset(&restored.sa_mask);
		if (recorded_[signal] && !inside_image(dispositions_[signal]))
			restored = dispositions_[signal];
		(void)sigaction(signal, &restored, nullptr);
	}

	/* The kernel blocks its timer signal and SIGPIPE on the thread which
	 * entered it; the host gets that thread back with the mask it lent. */
	if (mask_recorded_)
		(void)pthread_sigmask(SIG_SETMASK, &mask_, nullptr);

	/* The kernel gives that thread a signal stack it allocated itself. The
	 * thread has the stack it had again before the allocation is freed. */
	stack_t stack{};
	if (alternate_stack_recorded_ && sigaltstack(nullptr, &stack) == 0 &&
	    !same_stack(stack, alternate_stack_)) {
		stack_t restored = alternate_stack_;
		if ((restored.ss_flags & SS_DISABLE) != 0) {
			/* Darwin checks the size before it looks at SS_DISABLE. */
			restored = {};
			restored.ss_size = MINSIGSTKSZ;
			restored.ss_flags = SS_DISABLE;
		}
		(void)sigaltstack(&restored, nullptr);
	}

	/* The console descriptors come back with the flags and the terminal
	 * attributes they had, which the kernel's console channel changes. */
	for (std::size_t descriptor = 0; descriptor < standard_flags_.size();
	     ++descriptor) {
		if (standard_flags_[descriptor] < 0)
			continue;
		const int fd = static_cast<int>(descriptor);
		const int flags = fcntl(fd, F_GETFL);
		if (flags >= 0 &&
		    (flags & settable_status_flags) != standard_flags_[descriptor])
			(void)fcntl(fd, F_SETFL, standard_flags_[descriptor]);
		struct termios attributes {};
		if (standard_terminal_recorded_[descriptor] &&
		    tcgetattr(fd, &attributes) == 0 &&
		    !same_terminal(attributes, standard_terminals_[descriptor]))
			restore_terminal(fd, standard_terminals_[descriptor]);
	}
}

bool KernelSignalScope::inside_image(
	const struct sigaction &action) const noexcept
{
	const auto address = handler_address(action);
	return address >= image_begin_ && address < image_end_;
}

void release_kernel_image(void *address, std::size_t length,
	bool in_window) noexcept
{
	if (address == nullptr || address == MAP_FAILED || length == 0)
		return;
	if (::uml::memory::retain_borrowed_guest_ram(address, length))
		return;
	if (in_window)
		(void)::uml::memory::reserve_host_range(address, length);
	else
		(void)munmap(address, length);
}

int reserve_kernel_image(void *address, std::size_t length) noexcept
{
	return ::uml::memory::reserve_host_range(address, length);
}

bool keep_signed_kernel_text(void *address, std::size_t length) noexcept
{
	return ::uml::memory::keep_host_range(address, length);
}

bool signed_kernel_text_kept(const void *address, std::size_t length) noexcept
{
	return ::uml::memory::host_range_kept(address, length);
}

namespace {

/* Constant-initialized and trivially destructible: a carrier that outlives
 * the host's exit() still reads it, and the panic path reads it from a signal
 * handler. */
struct AliasTable {
	std::atomic<std::size_t> count;
	KernelTextAlias entries[kernel_text_alias_limit];
};
constinit AliasTable alias_table{};

} // namespace

bool record_kernel_text_alias(const void *address, std::size_t length,
	const void *carrier) noexcept
{
	const auto begin = reinterpret_cast<std::uintptr_t>(address);
	std::uintptr_t end;
	if (length == 0 || carrier == nullptr ||
	    __builtin_add_overflow(begin, length, &end))
		return false;
	const std::size_t count = alias_table.count.load(std::memory_order_acquire);
	for (std::size_t index = 0; index < count; ++index)
		if (alias_table.entries[index].begin == begin &&
		    alias_table.entries[index].end == end &&
		    alias_table.entries[index].carrier ==
			reinterpret_cast<std::uintptr_t>(carrier))
			return true;
	if (count == kernel_text_alias_limit)
		return false;
	alias_table.entries[count] = {begin, end,
		reinterpret_cast<std::uintptr_t>(carrier)};
	alias_table.count.store(count + 1, std::memory_order_release);
	return true;
}

std::size_t recorded_kernel_text_aliases(KernelTextAlias *aliases,
	std::size_t capacity) noexcept
{
	const std::size_t count = alias_table.count.load(std::memory_order_acquire);
	const std::size_t copied = count < capacity ? count : capacity;
	for (std::size_t index = 0; index < copied; ++index)
		aliases[index] = alias_table.entries[index];
	return count;
}

namespace {

/* Whether the __TEXT,__uml_elf section of the image `header` names is exactly
 * [image, image + size). */
[[nodiscard]] bool embedded_section_is(const mach_header_64 *header,
	const void *image, std::size_t size) noexcept
{
	if (header == nullptr)
		return false;
	unsigned long length = 0;
	const void *const start =
		getsectiondata(header, "__TEXT", "__uml_elf", &length);
	return start != nullptr && start == image && length == size;
}

} // namespace

bool is_embedded_kernel_image(const void *image, std::size_t size) noexcept
{
	if (image == nullptr || size == 0)
		return false;
	Dl_info library{};
	if (dladdr(reinterpret_cast<const void *>(&is_embedded_kernel_image),
		   &library) != 0 &&
	    embedded_section_is(
		    static_cast<const mach_header_64 *>(library.dli_fbase), image,
		    size))
		return true;
	return embedded_section_is(
		reinterpret_cast<const mach_header_64 *>(_NSGetMachExecuteHeader()),
		image, size);
}

} // namespace uml::runtime
