// SPDX-License-Identifier: GPL-2.0-or-later
/* A 16 KiB native slot, the host page, stays one shared ("direct") mapping
 * of its backing until a 4 KiB guest operation needs one quarter to differ.
 * The slot then becomes four private quarters built from a snapshot of it.
 * Another carrier can store through the direct slot meanwhile, and the store
 * must not be lost from the native view: the slot is read-only from the
 * snapshot until the private quarters replace it, and a conversion that fails
 * gives the slot its write access back.
 */
#include "allocation_fault.hpp"
#include "mach_vm_region_test_support.hpp"
#include "native_address_space.hpp"
#include "native_window_test_support.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"
#include "uml_darwin_memory.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr std::size_t guest_page_size = 4096;
constexpr std::size_t marker_offset = 23;
constexpr std::string_view backing_name = "uml-native-direct.XXXXXX";

using uml::memory::testing::AllocationSite;
using uml::testing::ScratchBacking;

using uml::testing::host_page;
using uml::testing::require;

[[nodiscard]] vm_prot_t protection_at(std::uintptr_t address)
{
	const auto region = uml::testing::region_containing(address);
	return region ? region->protection : VM_PROT_NONE;
}

[[nodiscard]] std::byte load(std::uintptr_t address) noexcept
{
	return *reinterpret_cast<volatile const std::byte *>(address);
}

void store(std::uintptr_t address, std::byte value) noexcept
{
	*reinterpret_cast<volatile std::byte *>(address) = value;
}

/* A range of the test window the slot is mapped into. */
class WindowSlot final {
public:
	WindowSlot()
		: address_{uml::testing::reserve_native_test_window(host_page())}
	{
		require(address_ != 0, "native window reservation failed");
		require(uml::testing::release_native_test_window(
			address_, host_page()),
			"native window reservation release failed");
	}

	~WindowSlot()
	{
		(void)munmap(reinterpret_cast<void *>(address_), host_page());
	}

	WindowSlot(const WindowSlot &) = delete;
	WindowSlot &operator=(const WindowSlot &) = delete;

	[[nodiscard]] std::uintptr_t address() const noexcept { return address_; }

private:
	std::uintptr_t address_{};
};

/* The first shadow-page checkpoint a conversion reaches falls after its
 * snapshot and before the private quarters replace the slot. The observer
 * acts there as another carrier would: it stores to a quarter the operation
 * does not touch, if the slot lets it. */
struct ConversionProbe final {
	std::atomic<std::uintptr_t> target{};
	std::atomic<bool> reached{};
	std::atomic<vm_prot_t> protection{VM_PROT_NONE};
	std::atomic<bool> stored{};
	std::byte value{};
};

ConversionProbe probe;

void observe_conversion(AllocationSite site) noexcept
{
	if (site != AllocationSite::native_mapping)
		return;
	const auto target = probe.target.exchange(0, std::memory_order_acq_rel);
	if (target == 0)
		return;
	const auto protection = protection_at(target);
	probe.protection.store(protection, std::memory_order_relaxed);
	probe.reached.store(true, std::memory_order_release);
	if ((protection & VM_PROT_WRITE) == 0)
		return;
	store(target, probe.value);
	probe.stored.store(true, std::memory_order_release);
}

void aStoreDuringTheConversionIsKept()
{
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	WindowSlot slot;
	auto space = uml::memory::native_address_space();
	require(space != nullptr, "native address space unavailable");
	const auto base = slot.address();
	const auto initial_direct = space->direct_mapping_count_for_testing();
	require(darwin_uml_vm_map(reinterpret_cast<void *>(base),
		backing.descriptor(), 0, host_page(), PROT_READ | PROT_WRITE,
		0, 1) == 0, "direct map failed");
	require(space->direct_mapping_count_for_testing() == initial_direct + 1,
		"an aligned map did not become direct");
	const auto sibling = base + 3 * guest_page_size + marker_offset;
	store(sibling, std::byte{0x11});

	probe.value = std::byte{0x5a};
	probe.target.store(sibling, std::memory_order_release);
	uml::memory::testing::set_allocation_observer(observe_conversion);
	const int protected_quarter = darwin_uml_vm_protect(
		reinterpret_cast<void *>(base + guest_page_size),
		guest_page_size, PROT_READ);
	uml::memory::testing::set_allocation_observer(nullptr);
	probe.target.store(0, std::memory_order_release);

	require(protected_quarter == 0, "the quarter protect failed");
	require(space->direct_mapping_count_for_testing() == initial_direct,
		"the quarter protect did not convert the direct slot");
	require(probe.reached.load(std::memory_order_acquire),
		"the conversion reached no shadow-page checkpoint");
	if (probe.stored.load(std::memory_order_acquire))
		require(load(sibling) == probe.value,
			"a store made during the conversion was lost from the "
			"native view");
	require((probe.protection.load(std::memory_order_relaxed) &
		VM_PROT_WRITE) == 0,
		"the direct slot stayed writable while it was converted");
	require((protection_at(sibling) & VM_PROT_WRITE) != 0,
		"the converted sibling quarter lost its write access");
	store(sibling, std::byte{0x22});
	require(load(sibling) == std::byte{0x22},
		"a store after the conversion did not land");
	require(darwin_uml_vm_drop(reinterpret_cast<void *>(
		base + 3 * guest_page_size), guest_page_size) == 0 &&
		backing.get_byte(3 * guest_page_size + marker_offset) ==
			std::byte{0x22},
		"dropping the converted quarter did not publish its store");
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(base),
		host_page()) == 0 &&
		space->direct_mapping_count_for_testing() == initial_direct,
		"unmapping the converted slot failed");
}

void aFailedConversionKeepsTheSlotWritable()
{
	ScratchBacking backing{backing_name, 4 * guest_page_size};
	WindowSlot slot;
	auto space = uml::memory::native_address_space();
	require(space != nullptr, "native address space unavailable");
	const auto base = slot.address();
	const auto initial_direct = space->direct_mapping_count_for_testing();
	const auto initial_mappings = space->mapping_count_for_testing();
	require(darwin_uml_vm_map(reinterpret_cast<void *>(base),
		backing.descriptor(), 0, host_page(), PROT_READ | PROT_WRITE,
		0, 1) == 0, "direct map failed");

	uml::memory::testing::fail_next_allocation(AllocationSite::native_mapping);
	errno = 0;
	const int protected_quarter = darwin_uml_vm_protect(
		reinterpret_cast<void *>(base + guest_page_size),
		guest_page_size, PROT_READ);
	const int error = errno;
	uml::memory::testing::clear_allocation_failure();
	require(protected_quarter == -1 && error == ENOMEM,
		"the injected conversion failure was not reported");
	require(space->direct_mapping_count_for_testing() == initial_direct + 1 &&
		space->mapping_count_for_testing() == initial_mappings,
		"a failed conversion changed the slot's ownership");
	require((protection_at(base) & (VM_PROT_READ | VM_PROT_WRITE)) ==
		(VM_PROT_READ | VM_PROT_WRITE),
		"a failed conversion left the direct slot without write access");
	store(base + 2 * guest_page_size + marker_offset, std::byte{0x33});
	require(backing.get_byte(2 * guest_page_size + marker_offset) ==
		std::byte{0x33},
		"a store after a failed conversion did not reach the backing");
	require(darwin_uml_vm_unmap(reinterpret_cast<void *>(base),
		host_page()) == 0 &&
		space->direct_mapping_count_for_testing() == initial_direct,
		"unmapping the direct slot failed");
}

} // namespace

int main()
{
	try {
		uml::testing::NativeTestWindowScope window;
		require(window.entered(), "could not configure the native window");
		aStoreDuringTheConversionIsKept();
		aFailedConversionKeepsTheSlotWritable();
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return EXIT_FAILURE;
	}
	std::puts("PASS: direct slot conversion keeps concurrent stores");
	return EXIT_SUCCESS;
}
