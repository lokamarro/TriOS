// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <termios.h>

namespace uml::runtime {

/*
 * An embedded kernel installs process-wide host signal handlers and owns
 * ITIMER_REAL while it runs, and all of it is still in place when the
 * kernel's entry returns. The loader releases the image right after that, so
 * a handler left pointing into it branches into unmapped memory the next time
 * its signal is delivered. That signal need not be a new one: Darwin assigns a
 * process-directed signal to a thread when it is generated and never moves it,
 * so a timer tick raised while every thread blocked SIGALRM waits on the main
 * thread for as long as that thread keeps it blocked, which on the AppKit
 * thread is until after the kernel has returned.
 *
 * A guest that restarts itself leaves the same process to the next boot, which
 * must find what the first one found. The kernel also leaves SIGPIPE, SIGIO
 * and SIGWINCH ignored, gives the thread that entered it a signal stack from
 * its own malloc, and makes the console descriptors non-blocking and its
 * terminal raw.
 *
 * The scope records the process's signal dispositions, the calling thread's
 * signal mask and signal stack, and the status flags and terminal attributes
 * of descriptors 0 to 2 before the kernel runs. Closing it disarms ITIMER_REAL,
 * passes every signal whose handler lies inside the image through SIG_IGN,
 * which discards an instance pending on any thread, and puts every changed
 * disposition back, whether the kernel pointed it into the image, ignored it
 * or set a handler of the host's. It restores the calling thread's mask and
 * signal stack, and the descriptors' flags and terminals. Only the status flags
 * F_SETFL can set are compared: Darwin reports a kernel-internal "was written"
 * bit in F_GETFL that no call clears. The scope must close before the image is
 * released and before the kernel's allocations are freed.
 */
class KernelSignalScope final {
public:
	KernelSignalScope(std::uintptr_t image_begin,
		std::uintptr_t image_end) noexcept;
	~KernelSignalScope();

	KernelSignalScope(const KernelSignalScope &) = delete;
	KernelSignalScope &operator=(const KernelSignalScope &) = delete;

private:
	[[nodiscard]] bool inside_image(
		const struct sigaction &action) const noexcept;

	std::uintptr_t image_begin_;
	std::uintptr_t image_end_;
	std::array<struct sigaction, NSIG> dispositions_{};
	std::array<bool, NSIG> recorded_{};
	sigset_t mask_{};
	bool mask_recorded_{};
	stack_t alternate_stack_{};
	bool alternate_stack_recorded_{};
	/* Descriptors 0 to 2. A flag value of -1 records nothing. */
	std::array<int, 3> standard_flags_{-1, -1, -1};
	std::array<struct termios, 3> standard_terminals_{};
	std::array<bool, 3> standard_terminal_recorded_{};
};

/*
 * Releases the kernel image the loader mapped, once the kernel has returned.
 * The image's last pages are the first guest RAM, and a host-service view of
 * guest RAM can still be read after the run: in-process backends stop only
 * after it returns. While such a view borrows the RAM root, the range stays
 * mapped and goes back to the window's reservation with the last borrowed
 * view, as retired guest RAM does. Otherwise a range inside the window goes
 * back to its reservation at once and any other range is unmapped. Signed
 * kernel text the host keeps (keep_signed_kernel_text) stays mapped either way.
 */
void release_kernel_image(void *address, std::size_t length,
	bool in_window) noexcept;

/* The loader's placement of the image inside the window: the range is replaced
 * by the window's reservation in place, except the signed kernel text a
 * previous load of the image left there, which no mapping can replace. 0, or
 * -1 with errno set. */
[[nodiscard]] int reserve_kernel_image(
	void *address, std::size_t length) noexcept;

/* Keep the signed kernel text the loader aliased at [address, address +
 * length) for the life of the process. iOS makes the alias permanent, and
 * macOS keeps it the same way (memory::keep_host_range): no release replaces
 * it, and the next load of the same image reuses it. False when it cannot be kept. */
[[nodiscard]] bool keep_signed_kernel_text(
	void *address, std::size_t length) noexcept;

/* Whether a previous load left exactly this signed kernel text in place. */
[[nodiscard]] bool signed_kernel_text_kept(
	const void *address, std::size_t length) noexcept;

/* One signed text range the loader aliased: [begin, end) reads the bytes of
 * the process's own __TEXT,__uml_elf section from `carrier` on. A return
 * address inside the alias names the same instruction at its carrier address,
 * which the crash reporter attributes to the image and the image's dSYM
 * symbolizes (kernel_crash_frames.hpp). */
struct KernelTextAlias {
	std::uintptr_t begin;
	std::uintptr_t end;
	std::uintptr_t carrier;
};

/* More executable segments than this are not recorded; their frames stay
 * unsymbolized. */
inline constexpr std::size_t kernel_text_alias_limit = 16;

/* Record that [address, address + length) aliases the carrier bytes at
 * `carrier`, once the alias is in place: the loader calls it before the kernel
 * runs, and the record lasts as long as the kept alias does. Recording the
 * same range again changes nothing. False when the table is full. */
bool record_kernel_text_alias(const void *address, std::size_t length,
	const void *carrier) noexcept;

/* Copy up to `capacity` recorded aliases to `aliases` and return how many
 * there are. Async-signal-safe: the panic path reads it. */
[[nodiscard]] std::size_t recorded_kernel_text_aliases(
	KernelTextAlias *aliases, std::size_t capacity) noexcept;

/* Whether [image, image + size) is exactly the kernel ELF the process links
 * into itself as its __TEXT,__uml_elf section: the section of the image that
 * holds this library (a framework, or the executable that links the library
 * statically), or of the main executable. That section is read-only and
 * file-backed, and it stays mapped for as long as its image does, so a caller
 * may run it in place instead of keeping a copy of it. Any other range is
 * the caller's own. */
[[nodiscard]] bool is_embedded_kernel_image(
	const void *image, std::size_t size) noexcept;

} // namespace uml::runtime
