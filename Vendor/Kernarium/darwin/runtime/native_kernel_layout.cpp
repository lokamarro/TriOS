// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_kernel_layout.hpp"

#include "host_environment.hpp"
#include "memory/mmu/address_layout.hpp"

#include <cstdio>
#include <mutex>

#include <sys/mman.h>
#include <unistd.h>

namespace uml::runtime {
namespace {

struct ProcessWindow final {
	std::mutex mutex;
	std::uintptr_t begin{};
	std::uintptr_t end{};
};

/* Never destroyed: a guest thread can still run when the process ends. */
[[clang::no_destroy]] constinit ProcessWindow process_window;

[[nodiscard]] NativeKernelLayout reserved_layout(std::uintptr_t base,
	std::size_t length) noexcept
{
	return {
		.load_bias = base - DARWIN_UML_NATIVE_LINK_ADDRESS,
		.window_start = base,
		.window_end = base + length,
		.valid = true,
		.reserved = true,
	};
}

/* A window is a multiple of the guest page, the host maps whole host pages,
 * and the reservation is what the host mapped. */
[[nodiscard]] std::size_t host_reservation_length(std::size_t length) noexcept
{
	const auto page = static_cast<std::size_t>(getpagesize());
	return (length + page - 1U) & ~(page - 1U);
}

/* Reserves [end, new_end) where the process's window ends, if the host has it
 * free; nothing is changed otherwise. */
[[nodiscard]] bool extend_reservation(std::uintptr_t end,
	std::uintptr_t new_end) noexcept
{
	const std::size_t length = new_end - end;
	void *const tail = mmap(reinterpret_cast<void *>(end), length, PROT_NONE,
		MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	if (tail == MAP_FAILED)
		return false;
	if (reinterpret_cast<std::uintptr_t>(tail) != end) {
		munmap(tail, length);
		return false;
	}
	return true;
}

} // namespace

NativeKernelPlacement native_kernel_placement(
	std::size_t guest_memory) noexcept
{
#if DARWIN_UML_NATIVE_HIGH_PLACEMENT
	return {MacosNativeKernelGeometry::derive_at(
			guest_memory, DARWIN_UML_MACOS_NATIVE_ADDRESS),
		WindowPlacement::exact};
#else
	/* The same iOS build runs on an Apple silicon Mac in Designed for iPad
	 * mode, where the process is a macOS process: the device window below
	 * the signed iOS envelope is already occupied there, and Darwin answers
	 * the loader's address hint with somewhere else entirely, which the
	 * loader must reject. Placement follows the address space the process
	 * actually has rather than the SDK it was built against.
	 */
	if (::uml::platform::running_ios_app_on_macos()) {
		return {MacosNativeKernelGeometry::derive_at(
				guest_memory, DARWIN_UML_MACOS_NATIVE_ADDRESS),
			WindowPlacement::exact};
	}
	return {DeviceNativeKernelGeometry::derive(guest_memory),
		WindowPlacement::anywhere};
#endif
}

NativeKernelLayout prepare_native_kernel_layout(
	std::size_t guest_memory) noexcept
{
	const auto product = native_kernel_placement(guest_memory);
	if (!product.geometry.valid)
		return product.geometry;
	const auto layout = reserve_process_native_window(
		product.geometry, product.placement);
	if (!layout.valid) {
		std::fprintf(stderr,
			"UML: cannot reserve a %llu-byte native kernel window\n",
			static_cast<unsigned long long>(
				product.geometry.window_end -
				product.geometry.window_start));
		return {.reservation_failed = true};
	}
	return layout;
}

NativeKernelLayout reserve_process_native_window(
	const NativeKernelLayout &geometry, WindowPlacement placement) noexcept
{
	if (!geometry.valid || geometry.window_end <= geometry.window_start)
		return {};
	const std::size_t length = static_cast<std::size_t>(
		geometry.window_end - geometry.window_start);
	std::lock_guard lock{process_window.mutex};
	const bool held = process_window.end != 0;
	/* A window the host places is the window wherever it is; one at an exact
	 * address must be at that address. */
	const bool fits_place = held &&
		(placement == WindowPlacement::anywhere ||
		 process_window.begin == geometry.window_start);
	if (fits_place &&
	    process_window.end - process_window.begin >= length)
		return reserved_layout(process_window.begin, length);
	if (fits_place && placement == WindowPlacement::exact) {
		const std::uintptr_t new_end = process_window.begin +
			host_reservation_length(length);
		if (!extend_reservation(process_window.end, new_end))
			return {};
		process_window.end = new_end;
		return reserved_layout(process_window.begin, length);
	}
	const auto window = reserve_native_kernel_window(geometry, placement);
	if (!window.valid)
		return {};
	/* The new reservation is held first, so a failure above leaves the old
	 * one. The old holds nothing: it was a window for a guest the process no
	 * longer has, handed back or never booted. */
	if (held)
		munmap(reinterpret_cast<void *>(process_window.begin),
			process_window.end - process_window.begin);
	process_window.begin = window.window_start;
	process_window.end = window.window_start + host_reservation_length(length);
	return window;
}

ProcessNativeWindow process_native_window() noexcept
{
	std::lock_guard lock{process_window.mutex};
	return {process_window.begin, process_window.end};
}

NativeKernelLayout reserve_native_kernel_window(
	const NativeKernelLayout &geometry, WindowPlacement placement) noexcept
{
	if (!geometry.valid || geometry.window_end <= geometry.window_start)
		return {};
	const std::size_t length = static_cast<std::size_t>(
		geometry.window_end - geometry.window_start);
	/* Reserve the window before the guest is brought up so nothing else can
	 * take part of it. The address the geometry names is tried first, so a
	 * host that still offers it keeps the established layout. iOS 27 does
	 * not: an app's usable virtual range moved, no hint inside the
	 * compile-time envelope is honoured any more, and the kernel image would
	 * be placed outside the window. Such a host is asked where a window of
	 * this size fits instead of being told. A macOS kernel is built for its
	 * address and gets no other.
	 *
	 * Nothing inside the window is released while it lives, not even for
	 * the kernel image: Darwin prefers a freshly freed range for its next
	 * unhinted allocation, and a host allocation that landed there would be
	 * replaced by the next MAP_FIXED guest mapping without either side being
	 * told. The loader and guest RAM map over the reservation in place.
	 */
	const std::size_t reservation = host_reservation_length(length);
	void *reserved = mmap(reinterpret_cast<void *>(geometry.window_start),
		reservation, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1,
		0);
	if (reserved != MAP_FAILED &&
	    reinterpret_cast<std::uintptr_t>(reserved) != geometry.window_start) {
		munmap(reserved, reservation);
		reserved = placement == WindowPlacement::exact ? MAP_FAILED :
			mmap(nullptr, reservation, PROT_NONE,
				MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	}
	if (reserved == MAP_FAILED)
		return {};
	const auto base = reinterpret_cast<std::uintptr_t>(reserved);
	if ((base & (DARWIN_UML_GUEST_PAGE_SIZE - 1U)) != 0U ||
	    base < DARWIN_UML_NATIVE_LINK_ADDRESS) {
		munmap(reserved, reservation);
		return {};
	}
	return reserved_layout(base, length);
}

NativeKernelLayoutScope::NativeKernelLayoutScope(
	const NativeKernelLayout &layout) noexcept
{
	if (!layout.valid)
		return;
	entered_ = ::uml::memory::configure_native_address_window(
		layout.window_start, layout.window_end);
}

NativeKernelLayoutScope::~NativeKernelLayoutScope()
{
	if (entered_)
		::uml::memory::reset_native_address_window();
}

} // namespace uml::runtime
