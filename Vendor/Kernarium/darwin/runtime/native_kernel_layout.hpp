// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace uml::runtime {

struct NativeKernelLayout final {
	std::uintptr_t load_bias{};
	std::uintptr_t window_start{};
	std::uintptr_t window_end{};
	bool valid{};
	/* The host already holds the whole window as one reservation, so the
	 * kernel image and guest RAM must be mapped over it in place. */
	bool reserved{};
	/* With `valid` false: the geometry fits, but the host would not reserve
	 * the window a physical iOS device requires. */
	bool reservation_failed{};
};

/* Where the kernel goes below an address ceiling: the macOS host's address
 * limit, or the signed iOS envelope. Everything else is the product's own
 * constant (UML::GuestPageConfiguration), including its 4 KiB guest page. */
template<std::uint64_t AddressCeiling>
class NativeKernelLayoutGeometry final {
	static constexpr std::uint64_t link_address =
		DARWIN_UML_NATIVE_LINK_ADDRESS;
	static constexpr std::uint64_t minimum_auxiliary =
		DARWIN_UML_NATIVE_AUXILIARY_MINIMUM;
	static constexpr std::uint64_t maximum_auxiliary =
		DARWIN_UML_NATIVE_AUXILIARY_MAXIMUM;
	static constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
	/* The kernel places its stub pages and the guarded vhost-user
	 * shared-memory aperture above the RAM-scaled auxiliary allowance, so
	 * the admitted native window must reserve that span too. Anything less
	 * leaves the kernel's own stub write outside the window the host will
	 * admit.
	 */
	static constexpr std::uint64_t reserved_auxiliary =
		DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION +
		DARWIN_UML_NATIVE_STUB_PAGES * page_size;
	static_assert((page_size & (page_size - 1U)) == 0);
	static_assert(minimum_auxiliary <= maximum_auxiliary);
	static_assert(((minimum_auxiliary | maximum_auxiliary |
		DARWIN_UML_NATIVE_VHOST_USER_SHMEM_RESERVATION) &
		(page_size - 1U)) == 0);
	static_assert(link_address <= AddressCeiling);
	static_assert(maximum_auxiliary <= AddressCeiling - link_address);
	static_assert(reserved_auxiliary <=
		AddressCeiling - link_address - maximum_auxiliary);

public:
	[[nodiscard]] static constexpr NativeKernelLayout derive(
		std::uint64_t guest_memory) noexcept
	{
		if (guest_memory == 0 ||
		    guest_memory > std::numeric_limits<std::uint64_t>::max() -
			(page_size - 1U))
			return {};
		const auto rounded = (guest_memory + page_size - 1U) &
			~(page_size - 1U);
		/* The auxiliary allowance tracks the page-rounded RAM
		 * one-for-one between its floor and its ceiling, as the kernel
		 * derives it. */
		const auto floored = rounded < minimum_auxiliary ?
			minimum_auxiliary : rounded;
		/* Capped so RAM plus the vmalloc allowance stays inside the
		 * signed iOS envelope; vmalloc's own consumption is bounded by
		 * the page allocator rather than by this reservation. The fixed
		 * kernel reservation is added after the cap because it is not
		 * part of that allowance. */
		const auto capped = floored > maximum_auxiliary ?
			maximum_auxiliary : floored;
		const auto auxiliary = capped + reserved_auxiliary;
		constexpr auto envelope = AddressCeiling - link_address;
		if (rounded == 0 || rounded > envelope ||
		    auxiliary > envelope - rounded)
			return {};
		const auto start = AddressCeiling - auxiliary - rounded;
		return {
			.load_bias = static_cast<std::uintptr_t>(start - link_address),
			.window_start = static_cast<std::uintptr_t>(start),
			.window_end = static_cast<std::uintptr_t>(AddressCeiling),
			.valid = true,
		};
	}

	[[nodiscard]] static constexpr NativeKernelLayout derive_at(
		std::uint64_t guest_memory, std::uint64_t start) noexcept
	{
		const auto packed = derive(guest_memory);
		if (!packed.valid || start < link_address ||
		    start > AddressCeiling)
			return {};
		const auto length = packed.window_end - packed.window_start;
		if (length > AddressCeiling - start)
			return {};
		return {
			.load_bias = static_cast<std::uintptr_t>(start - link_address),
			.window_start = static_cast<std::uintptr_t>(start),
			.window_end = static_cast<std::uintptr_t>(start + length),
			.valid = true,
		};
	}
};

/* Where the kernel goes in a macOS process. The low canonical range is
 * already spoken for there, so the image sits high, above every ordinary
 * allowance. */
using MacosNativeKernelGeometry =
	NativeKernelLayoutGeometry<DARWIN_UML_GUEST_ADDRESS_LIMIT>;
/* Where it goes on an iOS device, packed against the signed iOS envelope. */
using DeviceNativeKernelGeometry =
	NativeKernelLayoutGeometry<DARWIN_UML_NATIVE_ADDRESS_CEILING>;

/* How a window finds its place. */
enum class WindowPlacement : std::uint8_t {
	/* The geometry's address or nothing: a macOS process, whose address space
	 * is the host's to place a kernel built for it in. */
	exact,
	/* The geometry's address when the host still offers it, else wherever
	 * the host has room: a physical iOS device. */
	anywhere,
};

/* Where the product puts a guest of this size: the geometry of its window and
 * how the host finds the window its place. The geometry is invalid when
 * guest_memory does not fit the product's envelope. Nothing is reserved. */
struct NativeKernelPlacement final {
	NativeKernelLayout geometry;
	WindowPlacement placement{WindowPlacement::anywhere};
};
[[nodiscard]] NativeKernelPlacement native_kernel_placement(
	std::size_t guest_memory) noexcept;

/* Reserve a window of the geometry's length as one protectionless mapping.
 * The whole window stays reserved: the kernel image, its physical prefix and
 * guest RAM are all mapped over it in place. Returns an invalid layout when no
 * suitable window can be reserved. */
[[nodiscard]] NativeKernelLayout reserve_native_kernel_window(
	const NativeKernelLayout &geometry,
	WindowPlacement placement = WindowPlacement::anywhere) noexcept;

/* The process's window for a guest of this geometry.
 *
 * The window is one reservation for the life of the process, on every product:
 * the first call reserves it, the guest maps its kernel image, its RAM and its
 * auxiliary mappings over it with MAP_FIXED, and a later call, such as that of
 * a guest which restarted itself, returns the same window. It is never
 * unmapped while a guest may use it and no range of it is ever freed: Darwin
 * gives the next unhinted allocation the most recently freed range, so a host
 * allocation that landed inside would be replaced by the guest's next fixed
 * mapping, silently on both sides. The guest hands it back whole
 * (hand_back_anonymous_guest_ram), protectionless again around the signed
 * kernel text that iOS makes permanent and the next load reuses.
 *
 * A window longer than the one held, which only a process whose guest is not
 * spent asks for, extends the reservation at its exact address or, where the
 * host places the window, takes a new one and releases the old, which holds
 * nothing. Only one run uses the window at a time (the caller holds the
 * process address-space gate). Invalid when the host will not reserve it. */
[[nodiscard]] NativeKernelLayout reserve_process_native_window(
	const NativeKernelLayout &geometry, WindowPlacement placement) noexcept;

/* The layout for this process: the placement's window, reserved. It is invalid
 * when guest_memory does not fit the product's envelope, and with
 * `reservation_failed` set when the process cannot reserve its window. */
[[nodiscard]] NativeKernelLayout prepare_native_kernel_layout(
	std::size_t guest_memory) noexcept;

/* Where the process's window is, held whole; empty before the first
 * reservation. The window may be longer than the layout last prepared. */
struct ProcessNativeWindow final {
	std::uintptr_t begin{};
	std::uintptr_t end{};
};
[[nodiscard]] ProcessNativeWindow process_native_window() noexcept;

class NativeKernelLayoutScope final {
public:
	explicit NativeKernelLayoutScope(
		const NativeKernelLayout &layout) noexcept;
	~NativeKernelLayoutScope();

	NativeKernelLayoutScope(const NativeKernelLayoutScope &) = delete;
	NativeKernelLayoutScope &operator=(
		const NativeKernelLayoutScope &) = delete;

	[[nodiscard]] bool entered() const noexcept { return entered_; }

private:
	bool entered_{};
};

} // namespace uml::runtime
