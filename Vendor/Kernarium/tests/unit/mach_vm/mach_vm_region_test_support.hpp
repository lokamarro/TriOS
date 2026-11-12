// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <optional>
#include <stdexcept>
#include <unistd.h>

namespace uml::testing {

/* The host page, which is what mlock, mmap and the footprint count in. */
[[nodiscard]] inline std::size_t host_page() noexcept
{
	return static_cast<std::size_t>(getpagesize());
}

/* The task's physical footprint, which a released range leaves and a
 * committed one rejoins; 0 when the kernel will not report it. */
[[nodiscard]] inline std::uint64_t task_footprint() noexcept
{
	task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO,
		    reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
		return 0;
	return info.phys_footprint;
}

/* One map entry of this task, as mach_vm_region() reports it. */
struct VmRegion final {
	std::uintptr_t start{};
	std::size_t size{};
	vm_prot_t protection{};
	unsigned short user_wired_count{};
};

struct VmRegionQuery final {
	kern_return_t status{KERN_FAILURE};
	VmRegion region{};
};

/* The map entry that holds address or, when none does, the first one above
 * it; KERN_INVALID_ADDRESS when no entry lies at or above address. */
[[nodiscard]] inline VmRegionQuery query_vm_region(
	std::uintptr_t address) noexcept
{
	mach_vm_address_t start = address;
	mach_vm_size_t size = 0;
	vm_region_basic_info_data_64_t information{};
	mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t object = MACH_PORT_NULL;
	const auto status = mach_vm_region(mach_task_self(), &start, &size,
		VM_REGION_BASIC_INFO_64,
		reinterpret_cast<vm_region_info_t>(&information), &count,
		&object);
	if (object != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), object);
	if (status != KERN_SUCCESS)
		return {status, {}};
	return {status, {static_cast<std::uintptr_t>(start),
		static_cast<std::size_t>(size), information.protection,
		information.user_wired_count}};
}

/* The map entry that holds address, and nothing for an unmapped address,
 * whose query would report the entry above it. */
[[nodiscard]] inline std::optional<VmRegion> region_containing(
	std::uintptr_t address) noexcept
{
	const auto query = query_vm_region(address);
	if (query.status != KERN_SUCCESS || query.region.start > address ||
	    address - query.region.start >= query.region.size)
		return std::nullopt;
	return query.region;
}

[[nodiscard]] inline std::optional<VmRegion> region_containing(
	const void *pointer) noexcept
{
	return region_containing(reinterpret_cast<std::uintptr_t>(pointer));
}

/* Replace [address, address + length) with a zero-filled read/write mapping
 * that no later MAP_FIXED or vm_remap can replace, as iOS makes the loader's
 * aliases of the code-signed kernel text. A macOS process is exempt from that
 * rule for signed code, but XNU applies it to a mapping allocated permanent. */
[[nodiscard]] inline bool allocate_permanent(
	void *address, std::size_t length) noexcept
{
	auto target = reinterpret_cast<mach_vm_address_t>(address);
	return mach_vm_allocate(mach_task_self(), &target, length,
		       VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE |
			       VM_FLAGS_PERMANENT) == KERN_SUCCESS &&
		target == reinterpret_cast<mach_vm_address_t>(address);
}

/* The user wire count of the map entry holding pointer; 0 when none does. */
[[nodiscard]] inline unsigned wired_count(const void *pointer) noexcept
{
	const auto region = region_containing(pointer);
	return region ? region->user_wired_count : 0;
}

[[nodiscard]] inline vm_prot_t current_vm_protection(const void *pointer)
{
	const auto region = region_containing(pointer);
	if (!region)
		throw std::runtime_error{"cannot query Darwin VM protection"};
	return region->protection;
}

[[nodiscard]] inline bool writable_and_executable(vm_prot_t protection)
{
	return (protection & (VM_PROT_WRITE | VM_PROT_EXECUTE)) ==
		(VM_PROT_WRITE | VM_PROT_EXECUTE);
}

[[nodiscard]] inline bool process_has_writable_executable_region()
{
	std::uintptr_t cursor = 0;
	for (;;) {
		const auto query = query_vm_region(cursor);
		if (query.status == KERN_INVALID_ADDRESS)
			return false;
		if (query.status != KERN_SUCCESS || query.region.size == 0)
			throw std::runtime_error{"cannot enumerate Darwin VM protections"};
		if (writable_and_executable(query.region.protection))
			return true;
		if (query.region.start >
		    std::numeric_limits<std::uintptr_t>::max() - query.region.size)
			return false;
		cursor = query.region.start + query.region.size;
	}
}

} // namespace uml::testing
