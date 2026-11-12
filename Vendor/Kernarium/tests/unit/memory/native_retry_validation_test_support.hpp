// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "native_address_space.hpp"
#include "require.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mach/mach.h>
#include <sys/mman.h>
#include <utility>
#include <vector>

namespace uml::testing {
namespace detail {

class ScopedMachSendRight final {
public:
	explicit ScopedMachSendRight(mach_port_t name) noexcept : name_{name} {}
	~ScopedMachSendRight()
	{
		if (name_ != MACH_PORT_NULL)
			(void)mach_port_deallocate(mach_task_self(), name_);
	}

	ScopedMachSendRight(const ScopedMachSendRight &) = delete;
	ScopedMachSendRight &operator=(const ScopedMachSendRight &) = delete;

	[[nodiscard]] mach_port_t get() const noexcept { return name_; }

private:
	mach_port_t name_{MACH_PORT_NULL};
};

template<typename Element>
class ScopedVmArray final {
public:
	ScopedVmArray(Element *elements, mach_msg_type_number_t count) noexcept
		: elements_{elements}, count_{count}
	{
	}

	~ScopedVmArray()
	{
		if (elements_ != nullptr)
			(void)vm_deallocate(mach_task_self(),
				reinterpret_cast<vm_address_t>(elements_),
				static_cast<vm_size_t>(count_) * sizeof(Element));
	}

	ScopedVmArray(const ScopedVmArray &) = delete;
	ScopedVmArray &operator=(const ScopedVmArray &) = delete;

private:
	Element *elements_{};
	mach_msg_type_number_t count_{};
};

[[nodiscard]] inline ScopedMachSendRight mapping_object_name(
	std::uintptr_t address)
{
	vm_address_t region = static_cast<vm_address_t>(address);
	vm_size_t size = 0;
	vm_region_basic_info_data_64_t info{};
	mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t object = MACH_PORT_NULL;
	const auto result = vm_region_64(mach_task_self(), &region, &size,
		VM_REGION_BASIC_INFO_64,
		reinterpret_cast<vm_region_info_t>(&info), &count, &object);
	const bool contains = result == KERN_SUCCESS && region <= address &&
		size != 0 && address - region < size;
	if (!contains) {
		if (object != MACH_PORT_NULL) {
			(void)mach_port_deallocate(mach_task_self(), object);
			object = MACH_PORT_NULL;
		}
		require(false, "VM query did not contain the mapped retry address");
	}
	return ScopedMachSendRight{object};
}

[[nodiscard]] inline mach_port_urefs_t send_right_references(
	mach_port_t name)
{
	mach_port_urefs_t references = 0;
	require(mach_port_get_refs(mach_task_self(), name, MACH_PORT_RIGHT_SEND,
		&references) == KERN_SUCCESS,
		"could not query mapping object-name send-right references");
	return references;
}

using SendRightSnapshot =
	std::vector<std::pair<mach_port_name_t, mach_port_urefs_t>>;

[[nodiscard]] inline SendRightSnapshot send_right_snapshot()
{
	mach_port_name_array_t names = nullptr;
	mach_msg_type_number_t name_count = 0;
	mach_port_type_array_t types = nullptr;
	mach_msg_type_number_t type_count = 0;
	const auto result = mach_port_names(mach_task_self(), &names, &name_count,
		&types, &type_count);
	ScopedVmArray<mach_port_name_t> scoped_names{names, name_count};
	ScopedVmArray<mach_port_type_t> scoped_types{types, type_count};
	require(result == KERN_SUCCESS && name_count == type_count,
		"could not snapshot task Mach port rights");

	SendRightSnapshot snapshot;
	snapshot.reserve(name_count);
	for (mach_msg_type_number_t index = 0; index < name_count; ++index) {
		if ((types[index] & MACH_PORT_TYPE_SEND) == 0)
			continue;
		mach_port_urefs_t references = 0;
		require(mach_port_get_refs(mach_task_self(), names[index],
			MACH_PORT_RIGHT_SEND, &references) == KERN_SUCCESS,
			"could not query task Mach send-right references");
		snapshot.emplace_back(names[index], references);
	}
	std::sort(snapshot.begin(), snapshot.end());
	return snapshot;
}

} // namespace detail

inline void exercise_native_retry_direct(std::uintptr_t address,
	std::uintptr_t following_address, std::uintptr_t foreign_address,
	std::size_t host_page_size, int fd)
{
	using uml::memory::NativeAddressSpace;
	NativeAddressSpace space;

	require(space.retry_direct(address, false, false) == 0,
		"absent direct mapping authorized a retry");
	require(space.retry_direct(std::numeric_limits<std::uintptr_t>::max(),
		false, false) == 0,
		"maximum retry address wrapped instead of reporting safe absence");

	require(space.map_direct(address, fd, 0, host_page_size,
		PROT_READ | PROT_WRITE) == 0,
		"could not create direct retry fixture");
	require(space.retry_direct(address, false, false) == 1 &&
		space.retry_direct(address, false, true) == 1 &&
		space.retry_direct(address, true, false) == 0 &&
		space.retry_direct(address, true, true) == 0,
		"RW direct retry permissions were not exact");
	require(space.retry_direct(address + 1, false, true) == 1 &&
		space.retry_direct(address + host_page_size - 1,
			false, true) == 1,
		"direct retry rejected mapped interior or final-byte addresses");

	{
		auto object = detail::mapping_object_name(address);
		const auto task_rights_before = detail::send_right_snapshot();
		mach_port_urefs_t object_references = 0;
		if (object.get() != MACH_PORT_NULL)
			object_references =
				detail::send_right_references(object.get());
		for (unsigned int iteration = 0; iteration < 256; ++iteration) {
			const bool execute = (iteration & 2U) != 0;
			const bool write = (iteration & 1U) != 0;
			const int expected = execute ? 0 : 1;
			require(space.retry_direct(address, execute, write) == expected,
				"repeated direct retry permission result changed");
		}
		if (object.get() != MACH_PORT_NULL)
			require(detail::send_right_references(object.get()) ==
				object_references,
				"direct retries changed the mapping object-name urefs");
		require(detail::send_right_snapshot() == task_rights_before,
			"direct retries leaked Mach send rights or references");
	}

	require(space.protect_direct(address, host_page_size, PROT_READ) == 0 &&
		space.retry_direct(address, false, false) == 1 &&
		space.retry_direct(address, false, true) == 0 &&
		space.retry_direct(address, true, false) == 0 &&
		space.retry_direct(address, true, true) == 0,
		"read-only direct retry permissions were not exact");
	require(space.protect_direct(address, host_page_size,
		PROT_READ | PROT_EXEC) == 0 &&
		space.retry_direct(address, false, false) == 1 &&
		space.retry_direct(address, false, true) == 0 &&
		space.retry_direct(address, true, false) == 1 &&
		space.retry_direct(address, true, true) == 0,
		"RX direct retry permissions did not accumulate write and execute");
	require(space.protect_direct(address, host_page_size, PROT_NONE) == 0 &&
		space.retry_direct(address, false, false) == 0 &&
		space.retry_direct(address, false, true) == 0 &&
		space.retry_direct(address, true, false) == 0 &&
		space.retry_direct(address, true, true) == 0,
		"PROT_NONE direct mapping authorized a retry");

	require(space.protect_direct(address, host_page_size,
		PROT_READ | PROT_WRITE) == 0,
		"could not restore direct retry fixture protection");
	require(mprotect(reinterpret_cast<void *>(address), host_page_size,
		PROT_READ) == 0 &&
		space.retry_direct(address, false, false) == 1 &&
		space.retry_direct(address, false, true) == 0 &&
		space.retry_direct(address, true, false) == 0 &&
		space.retry_direct(address, true, true) == 0,
		"raw read-only mprotect drift was not observed by direct retry");
	require(mprotect(reinterpret_cast<void *>(address), host_page_size,
		PROT_NONE) == 0 &&
		space.retry_direct(address, false, false) == 0 &&
		space.retry_direct(address, false, true) == 0 &&
		space.retry_direct(address, true, false) == 0 &&
		space.retry_direct(address, true, true) == 0,
		"raw mprotect drift was not observed by direct retry");
	require(mprotect(reinterpret_cast<void *>(address), host_page_size,
		PROT_READ | PROT_WRITE) == 0 &&
		space.retry_direct(address, false, true) == 1,
		"raw mprotect restore was not observed by direct retry");

	require(munmap(reinterpret_cast<void *>(address), host_page_size) == 0,
		"could not punch direct retry mapping hole");
	void *following = mmap(reinterpret_cast<void *>(following_address),
		host_page_size, PROT_READ, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
	require(following == reinterpret_cast<void *>(following_address),
		"could not create following-region containment fixture");
	require(space.retry_direct(address, false, false) == 0,
		"retry accepted the region following an unmapped hole");
	require(munmap(following, host_page_size) == 0,
		"could not remove following-region containment fixture");
	void *restored = mmap(reinterpret_cast<void *>(address), host_page_size,
		PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, fd, 0);
	require(restored == reinterpret_cast<void *>(address),
		"could not restore raw-unmapped direct fixture");
	require(space.unmap_direct(address, host_page_size) == 0 &&
		space.retry_direct(address, false, false) == 0,
		"ordinary direct unmap retained retry authorization");

	NativeAddressSpace owner;
	NativeAddressSpace intruder;
	require(owner.map_direct(foreign_address, fd, 0, host_page_size,
		PROT_READ | PROT_WRITE) == 0,
		"could not create foreign direct ownership fixture");
	errno = 0;
	require(intruder.retry_direct(foreign_address, false, false) == -1 &&
		errno == EBUSY,
		"foreign direct owner was reported as absent");
	require(owner.unmap_direct(foreign_address, host_page_size) == 0,
		"could not clean foreign direct ownership fixture");

	require(space.map_direct(address, fd, 0, host_page_size, PROT_READ) == 0,
		"could not create retired direct retry fixture");
	space.retire();
	errno = 0;
	require(space.retry_direct(address, false, false) == -1 &&
		errno == ESHUTDOWN,
		"retired direct address space did not report ESHUTDOWN");
}

} // namespace uml::testing
