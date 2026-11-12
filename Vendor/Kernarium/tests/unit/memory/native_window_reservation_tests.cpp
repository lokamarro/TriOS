// SPDX-License-Identifier: GPL-2.0-or-later
/* The guest window is one reservation that guest pages are mapped over. These
 * tests hold it to the property the reservation exists for: no address inside
 * the window is ever free, so the host can never place an unrelated allocation
 * there, and a live guest mapping cannot be destroyed by an unrelated owner
 * releasing its own memory.
 */
#include "mach_vm_region_test_support.hpp"
#include "native_window_test_support.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"
#include "uml_darwin_memory.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>

namespace {

using uml::testing::host_page;
using uml::testing::require;

[[nodiscard]] std::string describe(std::uintptr_t address, std::size_t length)
{
	std::array<char, 96> text{};
	std::snprintf(text.data(), text.size(), "%#llx+%#llx",
		static_cast<unsigned long long>(address),
		static_cast<unsigned long long>(length));
	return std::string{text.data()};
}

[[nodiscard]] bool inside_window(std::uintptr_t address) noexcept
{
	return address >= uml::testing::native_test_window_start &&
		address < uml::testing::native_test_window_end;
}

/* The exact question the window exists to answer: may the host give this range
 * to something else? A fixed Mach allocation succeeds only over free address
 * space, so it answers without depending on any placement policy. */
[[nodiscard]] bool range_is_free(std::uintptr_t address, std::size_t length)
{
	mach_vm_address_t reservation = address;
	if (mach_vm_allocate(mach_task_self(), &reservation, length,
		    VM_FLAGS_FIXED) != KERN_SUCCESS)
		return false;
	require(reservation == address,
		"fixed allocation answered with another address");
	require(mach_vm_deallocate(mach_task_self(), reservation, length) ==
		KERN_SUCCESS, "could not release the probe allocation");
	return true;
}

struct RegionReport final {
	bool covers{};
	vm_prot_t protection{};
};

[[nodiscard]] RegionReport region_over(
	std::uintptr_t address, std::size_t length) noexcept
{
	const auto region = uml::testing::region_containing(address);
	if (!region || region->size - (address - region->start) < length)
		return {};
	return {true, region->protection};
}

/* An unrelated owner asking the host for memory. Darwin honours a free hint,
 * so hinting the hole makes the probe deterministic on a macOS host, whose own
 * anywhere-placement never reaches the high window this test configures. On a
 * device, where the window is placed among the system allocators, the host
 * needs no hint: its allocation hint follows the most recently freed range. */
class Interloper final {
public:
	Interloper(std::uintptr_t hint, std::size_t length) noexcept
		: length_{length}
	{
		address_ = mmap(reinterpret_cast<void *>(hint), length,
			PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	}

	~Interloper() { release(); }

	Interloper(const Interloper &) = delete;
	Interloper &operator=(const Interloper &) = delete;

	[[nodiscard]] bool valid() const noexcept
	{
		return address_ != MAP_FAILED;
	}

	[[nodiscard]] std::uintptr_t address() const noexcept
	{
		return reinterpret_cast<std::uintptr_t>(address_);
	}

	/* The ordinary release every host allocator performs on its own memory. */
	void release() noexcept
	{
		if (address_ != MAP_FAILED) {
			(void)munmap(address_, length_);
			address_ = MAP_FAILED;
		}
	}

private:
	void *address_{MAP_FAILED};
	std::size_t length_{};
};

using uml::testing::ScratchBacking;

constexpr std::string_view backing_name = "uml-window-reserve.XXXXXX";

/* The window as the product builds it: one reservation, no protections, no
 * committed pages, with guest pages mapped over it. */
class WindowArena final {
public:
	explicit WindowArena(std::size_t pages) : length_{pages * host_page()}
	{
		base_ = uml::testing::reserve_native_test_window(length_);
		require(base_ != 0, "could not place a window arena");
		require(inside_window(base_) &&
			inside_window(base_ + length_ - 1) &&
			(base_ % host_page()) == 0,
			"the window arena escaped the configured window");
		require(mmap(reinterpret_cast<void *>(base_), length_, PROT_NONE,
			MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1,
			0) == reinterpret_cast<void *>(base_),
			"could not reserve the window arena");
	}

	~WindowArena()
	{
		(void)uml::testing::release_native_test_window(base_, length_);
	}

	WindowArena(const WindowArena &) = delete;
	WindowArena &operator=(const WindowArena &) = delete;

	[[nodiscard]] std::uintptr_t page(std::size_t index) const noexcept
	{
		return base_ + index * host_page();
	}

private:
	std::uintptr_t base_{};
	std::size_t length_{};
};

void map_pages(const WindowArena &arena,
	const ScratchBacking &backing, std::size_t first, std::size_t count)
{
	require(darwin_uml_vm_map(reinterpret_cast<void *>(arena.page(first)),
		backing.descriptor(), first * host_page(), count * host_page(),
		PROT_READ | PROT_WRITE, 0, 1) == 0,
		"the guest could not map " + describe(arena.page(first),
			count * host_page()));
}

void unmap_pages(const WindowArena &arena, std::size_t first, std::size_t count)
{
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(arena.page(first)),
		count * host_page()) == 0,
		"the guest could not unmap " + describe(arena.page(first),
			count * host_page()));
}

void write_marker(std::uintptr_t page, std::uint8_t value) noexcept
{
	*reinterpret_cast<volatile std::uint8_t *>(page) = value;
}

[[nodiscard]] std::uint8_t read_marker(std::uintptr_t page) noexcept
{
	return *reinterpret_cast<volatile std::uint8_t *>(page);
}

/* 1. An address the guest has released is still the window's, not the host's. */
void test_released_window_pages_are_never_free()
{
	constexpr std::size_t arena_pages = 8;
	constexpr std::size_t hole_first = 2;
	constexpr std::size_t hole_pages = 4;
	const ScratchBacking backing{backing_name, arena_pages * host_page()};
	const WindowArena arena{arena_pages};
	map_pages(arena, backing, 0, arena_pages);
	unmap_pages(arena, hole_first, hole_pages);

	const auto hole = arena.page(hole_first);
	const auto hole_length = hole_pages * host_page();
	require(!range_is_free(hole, hole_length),
		"a released guest range is free address space: " +
			describe(hole, hole_length));
	for (std::size_t index = 0; index < hole_pages; ++index)
		require(!range_is_free(arena.page(hole_first + index), host_page()),
			"a released guest page is free address space: " +
				describe(arena.page(hole_first + index), host_page()));

	const auto region = region_over(hole, hole_length);
	require(region.covers && region.protection == VM_PROT_NONE,
		"a released guest range is not a protectionless reservation: " +
			describe(hole, hole_length));

	/* Assorted sizes, including the exact hole, which the host reuses most
	 * eagerly because it fits without splitting anything. */
	for (const auto pages : {std::size_t{1}, std::size_t{2}, hole_pages,
		     hole_pages * 2, hole_pages * 8}) {
		const Interloper interloper{hole, pages * host_page()};
		require(interloper.valid(),
			"the host refused an allocation of " +
				describe(0, pages * host_page()));
		require(!inside_window(interloper.address()),
			"the host placed an unrelated " +
				describe(0, pages * host_page()) +
				" allocation inside the guest window at " +
				describe(interloper.address(), 0));
	}

	/* The same question without a hint. A macOS host never draws from this
	 * window's band, so this cannot fail here; it states the invariant the
	 * device depends on, where the window is placed in that band. */
	for (int attempt = 0; attempt < 32; ++attempt) {
		const Interloper interloper{0, hole_length};
		require(interloper.valid(), "the host refused an allocation");
		require(!inside_window(interloper.address()),
			"the host placed an unhinted allocation inside the guest "
			"window at " + describe(interloper.address(), 0));
	}

	unmap_pages(arena, 0, arena_pages);
	require(!range_is_free(arena.page(0), arena_pages * host_page()),
		"a fully released window arena is free address space");
}

/* 2. The consequence the device sees: the guest loses a mapping the host
 * acknowledged, because someone else was given the same address and then
 * released it. */
void test_live_mapping_survives_an_unrelated_allocation_cycle()
{
	constexpr std::size_t arena_pages = 8;
	constexpr std::size_t hole_first = 2;
	constexpr std::size_t hole_pages = 4;
	const ScratchBacking backing{backing_name, arena_pages * host_page()};
	const WindowArena arena{arena_pages};
	map_pages(arena, backing, 0, arena_pages);
	for (std::size_t index = 0; index < arena_pages; ++index)
		write_marker(arena.page(index),
			static_cast<std::uint8_t>(0x40 + index));

	unmap_pages(arena, hole_first, hole_pages);

	const auto hole = arena.page(hole_first);
	const auto hole_length = hole_pages * host_page();
	Interloper interloper{hole, hole_length};
	require(interloper.valid(), "the host refused the interloper allocation");
	require(!inside_window(interloper.address()),
		"the host placed the interloper inside the guest window at " +
			describe(interloper.address(), hole_length));

	/* The guest maps the range again. MAP_FIXED replaces whatever is there
	 * and reports success either way, so this step is silent even when it
	 * has just overwritten another owner's memory. */
	map_pages(arena, backing, hole_first, hole_pages);
	for (std::size_t index = hole_first; index < hole_first + hole_pages;
	     ++index)
		write_marker(arena.page(index),
			static_cast<std::uint8_t>(0x80 + index));

	interloper.release();

	for (std::size_t index = 0; index < arena_pages; ++index) {
		const auto page = arena.page(index);
		require(!range_is_free(page, host_page()),
			"an unrelated owner's release destroyed the guest mapping "
			"at " + describe(page, host_page()));
		const auto expected = static_cast<std::uint8_t>(
			index >= hole_first && index < hole_first + hole_pages ?
				0x80 + index : 0x40 + index);
		require(read_marker(page) == expected,
			"the guest mapping at " + describe(page, host_page()) +
				" no longer holds its own memory");
	}

	unmap_pages(arena, 0, arena_pages);
}

} // namespace

int main()
{
	try {
		const uml::testing::NativeTestWindowScope window;
		require(window.entered(),
			"could not configure the native address window");
		test_released_window_pages_are_never_free();
		test_live_mapping_survives_an_unrelated_allocation_cycle();
		std::puts("native window reservation tests passed");
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "native window reservation test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
}
