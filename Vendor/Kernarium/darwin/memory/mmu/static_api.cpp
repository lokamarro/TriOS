// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_guest.h"
#include "uml_darwin_memory.h"
#include "uml_darwin_static_mmu.h"
#include "uml/tlb_sync_ranges.h"
#include "address_layout.hpp"
#include "memory/guest_ram_commitment.hpp"
#if UML_MEMORY_HAS_NATIVE_VM
#include "bridge_call.hpp"
#include "native_address_space.hpp"
#endif

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#if UML_MEMORY_HAS_NATIVE_VM
#include <sys/mman.h>
#endif

namespace {

using uml::memory::AddressDomain;
using uml::memory::ConfiguredAddressLayout;

[[nodiscard]] AddressDomain address_range(
	const void *pointer, std::uint64_t length) noexcept
{
	const auto address = reinterpret_cast<std::uintptr_t>(pointer);
	return ConfiguredAddressLayout::classify(address, length);
}

[[nodiscard]] int invalid_address_range() noexcept
{
	errno = EINVAL;
	return -1;
}

#if UML_MEMORY_HAS_NATIVE_VM
[[nodiscard]] bool use_native_shadow(
	const uml::memory::NativeAddressSpace &space, const void *address,
	std::uint64_t offset, std::uint64_t length)
{
	return uml::memory::NativeAddressSpace::needs_shadow(
			reinterpret_cast<std::uintptr_t>(address), offset, length) ||
		space.contains_shadow(
			reinterpret_cast<std::uintptr_t>(address), length);
}

[[nodiscard]] int write_xor_execute(int protection) noexcept
{
	if ((protection & (PROT_WRITE | PROT_EXEC)) == (PROT_WRITE | PROT_EXEC))
		protection &= ~PROT_EXEC;
	return protection;
}
template<typename Function>
[[nodiscard]] int native_address_call(
	const std::shared_ptr<uml::memory::NativeAddressSpace> &space,
	Function &&function) noexcept
{
	return uml::memory::bridge_call(
		[&]() -> const auto & { return space; },
		std::forward<Function>(function), [] {});
}
#else
[[nodiscard]] int native_vm_unavailable() noexcept
{
	errno = ENOTSUP;
	return -1;
}
#endif

[[nodiscard]] int publish_native_shadow(
	int fd, std::uint64_t offset, std::uint64_t length) noexcept
{
#if UML_MEMORY_HAS_NATIVE_VM
	auto native_space = uml::memory::native_address_space();
	const auto result = native_address_call(native_space, [&](auto &space) {
		return space.publish_backing(fd, offset, length);
	});
	if (result == 0)
		std::atomic_thread_fence(std::memory_order_release);
	return result;
#else
	(void)fd;
	(void)offset;
	(void)length;
	return 0;
#endif
}

} // namespace

extern "C" int darwin_uml_vm_map(void *address, int fd, unsigned long long offset,
				 unsigned long length, int protection, int guest_code,
				 int guest_writable)
{
	const auto range = address_range(address, length);
	if (range == AddressDomain::invalid)
		return invalid_address_range();
	if (range == AddressDomain::guest) {
		if (publish_native_shadow(fd, offset, length) < 0)
			return -1;
		return darwin_uml_static_vm_map(address, fd, offset, length, protection,
						guest_code, guest_writable);
	}
#if UML_MEMORY_HAS_NATIVE_VM
	auto native_space = uml::memory::native_address_space();
	return native_address_call(native_space, [&](auto &space) {
		const bool shadow = use_native_shadow(
			space, address, offset, length);
		const bool exact = !uml::memory::NativeAddressSpace::needs_shadow(
			reinterpret_cast<std::uintptr_t>(address), offset, length);
		if (shadow && !exact)
			return space.map(
				reinterpret_cast<std::uintptr_t>(address), fd, offset,
				length, protection);
		/* Whole host pages of one backing map shared, replacing any
		 * shadow; one still executing this call keeps its shadow. */
		const int direct_protection = write_xor_execute(protection);
		const auto result = space.map_direct(
			reinterpret_cast<std::uintptr_t>(address), fd, offset,
			length, direct_protection);
		if (result < 0 && shadow && errno == EBUSY)
			return space.map(
				reinterpret_cast<std::uintptr_t>(address), fd, offset,
				length, protection);
		return result;
	});
#else
	return native_vm_unavailable();
#endif
}

extern "C" int darwin_uml_vm_map_generation(unsigned long generation, void *address,
					    int fd, unsigned long long offset,
					    unsigned long length, int protection,
					    int guest_code, int guest_writable)
{
	const auto range = address_range(address, length);
	if (range == AddressDomain::invalid)
		return invalid_address_range();
	if (range == AddressDomain::native)
		return darwin_uml_vm_map(address, fd, offset, length, protection,
					 guest_code, guest_writable);
	/* Physical aliases use private 4 KiB shadows on a 16 KiB Darwin host.
	 * Publish them before the software mapping and its code-change sequence
	 * become visible to an execution engine. */
	if (publish_native_shadow(fd, offset, length) < 0)
		return -1;
	return darwin_uml_static_vm_map_generation(generation, address, fd, offset,
						   length, protection, guest_code,
						   guest_writable);
}

extern "C" int darwin_uml_vm_protect(void *address, unsigned long length,
				     int protection)
{
	/* Guest execute permission is software-MMU metadata and may coexist with
	 * guest write permission.  Only sanitize the native Darwin mapping that
	 * is actually subject to the host W^X policy.
	 */
	const auto range = address_range(address, length);
	if (range == AddressDomain::invalid)
		return invalid_address_range();
	if (range == AddressDomain::guest)
		return darwin_uml_static_vm_protect(address, length, protection);
#if UML_MEMORY_HAS_NATIVE_VM
	auto native_space = uml::memory::native_address_space();
	return native_address_call(native_space, [&](auto &space) {
		const bool shadow = space.contains_shadow(
			reinterpret_cast<std::uintptr_t>(address), length);
		if (shadow || uml::memory::NativeAddressSpace::needs_shadow(
				reinterpret_cast<std::uintptr_t>(address), 0, length))
			return space.protect(
				reinterpret_cast<std::uintptr_t>(address), length,
				protection);
		return space.protect_direct(
			reinterpret_cast<std::uintptr_t>(address), length,
			write_xor_execute(protection));
	});
	return native_address_call(native_space, [&](auto &space) {
		return space.protect_direct(
			reinterpret_cast<std::uintptr_t>(address), length,
			write_xor_execute(protection));
	});
#else
	return native_vm_unavailable();
#endif
}

extern "C" int darwin_uml_vm_unmap(void *address, unsigned long length)
{
	const auto range = address_range(address, length);
	if (range == AddressDomain::invalid)
		return invalid_address_range();
	if (range == AddressDomain::guest)
		return darwin_uml_static_vm_unmap(address, length);
#if UML_MEMORY_HAS_NATIVE_VM
	auto native_space = uml::memory::native_address_space();
	return native_address_call(native_space, [&](auto &space) {
		const bool shadow = space.contains_shadow(
			reinterpret_cast<std::uintptr_t>(address), length);
		if (shadow || uml::memory::NativeAddressSpace::needs_shadow(
				reinterpret_cast<std::uintptr_t>(address), 0, length))
			return space.unmap(
				reinterpret_cast<std::uintptr_t>(address), length);
		return space.unmap_direct(
			reinterpret_cast<std::uintptr_t>(address), length);
	});
	return native_address_call(native_space, [&](auto &space) {
		return space.unmap_direct(
			reinterpret_cast<std::uintptr_t>(address), length);
	});
#else
	return native_vm_unavailable();
#endif
}

extern "C" int darwin_uml_vm_unmap_generation(unsigned long generation, void *address,
					      unsigned long length)
{
	const auto range = address_range(address, length);
	if (range == AddressDomain::invalid)
		return invalid_address_range();
	if (range == AddressDomain::guest)
		return darwin_uml_static_vm_unmap_generation(generation, address,
							     length);
#if UML_MEMORY_HAS_NATIVE_VM
	auto native_space = uml::memory::native_address_space();
	return native_address_call(native_space, [&](auto &space) {
		if (space.contains_shadow(
				reinterpret_cast<std::uintptr_t>(address), length) ||
		    uml::memory::NativeAddressSpace::needs_shadow(
				reinterpret_cast<std::uintptr_t>(address), 0, length))
			return space.unmap(
				reinterpret_cast<std::uintptr_t>(address), length);
		return space.unmap_direct(
			reinterpret_cast<std::uintptr_t>(address), length);
	});
	return native_address_call(native_space, [&](auto &space) {
		return space.unmap_direct(
			reinterpret_cast<std::uintptr_t>(address), length);
	});
#else
	return native_vm_unavailable();
#endif
}

extern "C" int darwin_uml_vm_drop(void *address, unsigned long length)
{
	const auto range = address_range(address, length);
	if (range == AddressDomain::invalid)
		return invalid_address_range();
	if (range == AddressDomain::guest)
		return 0;
#if UML_MEMORY_HAS_NATIVE_VM
	if ((reinterpret_cast<std::uintptr_t>(address) |
	     static_cast<std::uintptr_t>(length)) &
	    (DARWIN_UML_GUEST_PAGE_SIZE - 1))
		return invalid_address_range();
#endif
#if UML_MEMORY_HAS_NATIVE_VM
	auto native_space = uml::memory::native_address_space();
	return native_address_call(native_space, [&](auto &space) {
		if (space.contains_shadow(
				reinterpret_cast<std::uintptr_t>(address), length))
			return space.flush(
				reinterpret_cast<std::uintptr_t>(address), length);
		return space.flush_direct(
			reinterpret_cast<std::uintptr_t>(address), length);
	});
	return native_address_call(native_space, [&](auto &space) {
		return space.flush_direct(
			reinterpret_cast<std::uintptr_t>(address), length);
	});
#else
	(void)length;
	return native_vm_unavailable();
#endif
}

extern "C" int darwin_uml_vm_activate(unsigned long generation)
{
	return darwin_uml_static_vm_activate(generation);
}

extern "C" int darwin_uml_vm_batch_begin(unsigned long generation)
{
	return darwin_uml_static_vm_batch_begin(generation);
}

extern "C" int darwin_uml_vm_batch_end(void)
{
	/* The static API reports a rejected batch as Darwin's EAGAIN, which the
	 * kernel cannot tell from its own errno values. */
	const int result = darwin_uml_static_vm_batch_end();
	return result == -EAGAIN ? DARWIN_UML_VM_BATCH_REJECTED : result;
}

extern "C" int darwin_uml_vm_walker_lock(unsigned long generation)
{
	return darwin_uml_static_vm_walker_lock(generation);
}

extern "C" int darwin_uml_vm_walker_unlock(unsigned long generation)
{
	return darwin_uml_static_vm_walker_unlock(generation);
}

extern "C" int darwin_uml_vm_destroy(unsigned long generation)
{
	return darwin_uml_static_vm_destroy(generation);
}

extern "C" int darwin_uml_vm_flush_generation(unsigned long generation)
{
	return darwin_uml_static_vm_flush_generation(generation);
}
extern "C" int darwin_uml_vm_sync_from_backing(unsigned long address)
{
	const auto range = address_range(reinterpret_cast<void *>(address),
		DARWIN_UML_GUEST_PAGE_SIZE);
	if (range == AddressDomain::invalid)
		return invalid_address_range();
#if UML_MEMORY_HAS_NATIVE_VM
	if (range == AddressDomain::guest)
		return 0;
	auto native_space = uml::memory::native_address_space();
	/* Kernel uaccess asks for every page it touches, nearly always one the
	 * space maps directly, so that answer comes from one hold of its lock. */
	return native_address_call(native_space, [&](auto &space) {
		const auto direct = space.sync_direct_unless_shadow(
			address, DARWIN_UML_GUEST_PAGE_SIZE);
		if (direct <= 0)
			return direct;
#if DARWIN_UML_X86_64_USERSPACE_ONLY
		return space.reload_from_backing(
			address, DARWIN_UML_GUEST_PAGE_SIZE);
#else
		return space.refresh(address, DARWIN_UML_GUEST_PAGE_SIZE);
#endif
	});
	return native_address_call(native_space, [&](auto &space) {
		return space.sync_direct(
			address, DARWIN_UML_GUEST_PAGE_SIZE);
	});
#else
	(void)address;
	return 0;
#endif
}

extern "C" int darwin_uml_vm_sync_from_buffer(unsigned long address, const void *buffer,
					      unsigned long length)
{
	if (address_range(reinterpret_cast<void *>(address), length) ==
	    AddressDomain::invalid)
		return invalid_address_range();
	return darwin_uml_static_vm_copy_kernel_write(address, buffer, length);
}

extern "C" int darwin_uml_vm_invalidate_code(unsigned long generation,
					    unsigned long address,
					    unsigned long length)
{
	const auto range = address_range(reinterpret_cast<void *>(address),
		length);
	if (range == AddressDomain::invalid)
		return invalid_address_range();
	/* Only translated guest pages cache code; native pages execute the
	 * shared frame directly. */
	if (range != AddressDomain::guest)
		return 0;
	return darwin_uml_static_vm_invalidate_code_generation(generation,
		address, length);
}

/* The uaccess reaches the page through the kernel's own alias, which it
 * refreshes and writes back itself on 4 KiB, and installs nothing here, so no
 * 4 KiB native shadow needs publishing first: the walker that installs the
 * page later publishes it, as every software map does. */
extern "C" int darwin_uml_vm_discard_if_stale(unsigned long address, int fd,
					      unsigned long long offset)
{
	return darwin_uml_static_vm_discard_if_stale(address, fd, offset);
}

extern "C" int darwin_uml_vm_retry_fault(unsigned long address, int execute, int write)
{
	if (ConfiguredAddressLayout::is_guest(address))
		return darwin_uml_static_vm_retry_fault(address, execute, write);
#if UML_MEMORY_HAS_NATIVE_VM
	auto native_space = uml::memory::native_address_space();
	return native_address_call(native_space, [&](auto &space) {
		if (space.contains_shadow(address, 1))
			return space.retry(
				address, execute != 0, write != 0);
		return space.retry_direct(
			address, execute != 0, write != 0);
	});
	return native_address_call(native_space, [&](auto &space) {
		return space.retry_direct(
			address, execute != 0, write != 0);
	});
#else
	return 0;
#endif
}

extern "C" int darwin_uml_physmem_release(unsigned long long offset,
	unsigned long long length)
{
	return uml::memory::release_guest_ram(offset, length);
}

extern "C" int darwin_uml_physmem_commit(unsigned long long offset,
	unsigned long long length)
{
	return uml::memory::commit_guest_ram(offset, length);
}
