// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "mach_vm_region_test_support.hpp"
#include "memory/mmu/address_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <mach/mach.h>
#include <mach/mach_vm.h>

namespace uml::testing {

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UML_NATIVE_TEST_WINDOW_UNDER_TSAN 1
#endif
#if __has_feature(address_sanitizer)
#define UML_NATIVE_TEST_WINDOW_UNDER_ASAN 1
#endif
#endif

/* Match the product placement: macOS avoids crowded low host VM bands. On
 * arm64 macOS a sanitizer holds the product band itself: AddressSanitizer
 * keeps its allocator heap at 0x600000000000, and ThreadSanitizer reserves
 * everything outside its application ranges. A sanitized test takes a window
 * at the start of ThreadSanitizer's middle application range, 0x550000000000
 * to 0x5a0000000000, which AddressSanitizer leaves to the application too. */
#if defined(UML_NATIVE_TEST_WINDOW_UNDER_TSAN) || \
	defined(UML_NATIVE_TEST_WINDOW_UNDER_ASAN)
inline constexpr std::uint64_t native_test_window_start = 0x550000000000ULL;
inline constexpr std::uint64_t native_test_window_end =
	native_test_window_start +
	(DARWIN_UML_NATIVE_ADDRESS_END - DARWIN_UML_NATIVE_ADDRESS_START);
#elif DARWIN_UML_NATIVE_HIGH_PLACEMENT
inline constexpr std::uint64_t native_test_window_start =
	DARWIN_UML_MACOS_NATIVE_ADDRESS;
inline constexpr std::uint64_t native_test_window_end =
	native_test_window_start +
	(DARWIN_UML_NATIVE_ADDRESS_END - DARWIN_UML_NATIVE_ADDRESS_START);
#else
inline constexpr std::uint64_t native_test_window_start =
	DARWIN_UML_NATIVE_ADDRESS_START;
inline constexpr std::uint64_t native_test_window_end =
	DARWIN_UML_NATIVE_ADDRESS_END;
#endif

static_assert(native_test_window_start >= DARWIN_UML_NATIVE_ADDRESS_START);
static_assert(native_test_window_end <= DARWIN_UML_GUEST_ADDRESS_LIMIT);

class NativeTestWindowScope final {
public:
	NativeTestWindowScope() noexcept
		: entered_{::uml::memory::configure_native_address_window(
			native_test_window_start, native_test_window_end)}
	{
	}

	~NativeTestWindowScope()
	{
		if (entered_)
			::uml::memory::reset_native_address_window();
	}

	NativeTestWindowScope(const NativeTestWindowScope &) = delete;
	NativeTestWindowScope &operator=(const NativeTestWindowScope &) = delete;

	[[nodiscard]] bool entered() const noexcept { return entered_; }

private:
	bool entered_{};
};

[[nodiscard]] inline std::uintptr_t reserve_native_test_window(
	std::size_t length) noexcept
{
	constexpr auto start = native_test_window_start;
	constexpr auto end = native_test_window_end;
	if (length == 0 || length > end - start)
		return 0;

	const auto reserve_gap = [length](mach_vm_address_t gap_start,
		mach_vm_address_t gap_end) noexcept -> std::uintptr_t {
		if (gap_end < gap_start || length > gap_end - gap_start)
			return 0;
		const auto candidate = gap_end - length;
		mach_vm_address_t reservation = candidate;
		if (mach_vm_allocate(mach_task_self(), &reservation, length,
			    VM_FLAGS_FIXED) == KERN_SUCCESS) {
			if (reservation == candidate)
				return static_cast<std::uintptr_t>(reservation);
			(void)mach_vm_deallocate(
				mach_task_self(), reservation, length);
		}
		return 0;
	};

	mach_vm_address_t cursor = start;
	while (cursor < end) {
		const auto query = query_vm_region(cursor);
		if (query.status == KERN_INVALID_ADDRESS)
			return reserve_gap(cursor, end);
		if (query.status != KERN_SUCCESS)
			return 0;
		const mach_vm_address_t region = query.region.start;
		const mach_vm_size_t region_size = query.region.size;

		const auto gap_end = region < end ? region : end;
		if (const auto reserved = reserve_gap(cursor, gap_end);
		    reserved != 0)
			return reserved;
		if (region >= end || region_size > end - region)
			return 0;
		const auto next = region + region_size;
		if (next <= cursor)
			return 0;
		cursor = next;
	}
	return 0;
}

[[nodiscard]] inline bool release_native_test_window(
	std::uintptr_t address, std::size_t length) noexcept
{
	return mach_vm_deallocate(mach_task_self(), address, length) ==
		KERN_SUCCESS;
}

/* A window range the guest has released. The guest window never hands address
 * space back to the process, so a released range is the window's own
 * protectionless reservation: no mapping the guest can fault on, and no free
 * gap the host can give to an unrelated allocation. A range that was never
 * mapped stays absent instead, which is what the absence probes still mean. */
[[nodiscard]] inline bool released_into_native_window(
	std::uintptr_t address, std::size_t length) noexcept
{
	mach_vm_address_t cursor = address;
	const mach_vm_address_t end = address + length;
	while (cursor < end) {
		/* A cursor outside every region is a free gap, and a region
		 * with any protection is still a live mapping. */
		const auto region = region_containing(
			static_cast<std::uintptr_t>(cursor));
		if (!region || region->protection != VM_PROT_NONE)
			return false;
		cursor = region->start + region->size;
	}
	return true;
}

} // namespace uml::testing
