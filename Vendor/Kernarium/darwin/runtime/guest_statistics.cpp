// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_statistics.hpp"

#include "cpu/gadget_cache_budget.hpp"
#include "memory/domain_heap.hpp"
#include "memory/guest_ram_commitment.hpp"
#include "platform/gpu_memory_figures.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <mutex>

namespace uml::runtime {

namespace {

/* Leaf: nothing is taken under it. */
std::mutex ram_retirement_mutex;

constexpr std::string_view carrier_prefix = "uml-vcpu-";
constexpr std::string_view carrier_suffix = "-carrier";

} // namespace

bool is_vcpu_carrier_name(std::string_view name) noexcept
{
	if (!name.starts_with(carrier_prefix) || !name.ends_with(carrier_suffix) ||
	    name.size() <= carrier_prefix.size() + carrier_suffix.size())
		return false;
	name.remove_prefix(carrier_prefix.size());
	name.remove_suffix(carrier_suffix.size());
	for (const char digit : name)
		if (digit < '0' || digit > '9')
			return false;
	return true;
}

GuestCpuTime guest_cpu_time() noexcept
{
	thread_act_array_t threads = nullptr;
	mach_msg_type_number_t count = 0;
	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
		return {};
	GuestCpuTime total;
	total.listed = true;
	for (mach_msg_type_number_t index = 0; index < count; ++index) {
		thread_extended_info_data_t information {};
		mach_msg_type_number_t information_count = THREAD_EXTENDED_INFO_COUNT;
		if (thread_info(threads[index], THREAD_EXTENDED_INFO,
			    reinterpret_cast<thread_info_t>(&information),
			    &information_count) == KERN_SUCCESS &&
		    is_vcpu_carrier_name(std::string_view {information.pth_name,
			    ::strnlen(information.pth_name,
				    sizeof(information.pth_name))})) {
			++total.carriers;
			total.nanoseconds +=
				information.pth_user_time + information.pth_system_time;
		}
		(void)mach_port_deallocate(mach_task_self(), threads[index]);
	}
	(void)vm_deallocate(mach_task_self(),
		reinterpret_cast<vm_address_t>(threads),
		count * sizeof(thread_act_t));
	return total;
}

std::optional<GuestRamUsage> guest_ram_usage() noexcept
{
	/* A hand-back in progress holds the lock: the RAM is being given back,
	 * and the reader does not wait for it. */
	const std::unique_lock lock {ram_retirement_mutex, std::try_to_lock};
	if (!lock.owns_lock())
		return std::nullopt;
	const ::uml::memory::GuestRamCommitment *const commitment =
		::uml::memory::guest_ram_commitment();
	if (commitment == nullptr)
		return std::nullopt;
	const std::uint64_t ram = commitment->size();
	const std::uint64_t released = commitment->released_bytes();
	return GuestRamUsage {.ram_bytes = ram,
		.held_bytes = released < ram ? ram - released : 0U};
}

GuestRamRetirementScope::GuestRamRetirementScope() noexcept
{
	ram_retirement_mutex.lock();
}

GuestRamRetirementScope::~GuestRamRetirementScope()
{
	ram_retirement_mutex.unlock();
}

MemoryTerms memory_terms(bool read_malloc) noexcept
{
	MemoryTerms terms;
	if (const auto ram = guest_ram_usage())
		terms.guest_held = ram->held_bytes;
	if (const auto gpu = ::uml::platform::publishedGpuMemory();
	    gpu.budgetBytes != 0) {
		terms.gpu_budget = gpu.budgetBytes;
		terms.gpu_held = gpu.heldBytes;
		if (gpu.heldAtMs != 0)
			terms.gpu_age_ms = std::max<std::int64_t>(
				::uml::platform::gpuMemoryClockMs() - gpu.heldAtMs, 0);
	}
	const auto &cache = ::uml::cpu::gadget_cache::ledger();
	terms.cache = cache.open.load(std::memory_order_relaxed) +
		cache.shedding.load(std::memory_order_relaxed);
	terms.heaps = ::uml::memory::HeapRegion::instance().committed();
	if (read_malloc) {
		malloc_statistics_t zones{};
		malloc_zone_statistics(nullptr, &zones);
		terms.malloc_allocated = zones.size_allocated;
		terms.malloc_in_use = zones.size_in_use;
	}
	task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO,
		    reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
		const auto positive = [](std::int64_t value) {
			return static_cast<std::uint64_t>(std::max<std::int64_t>(value, 0));
		};
		terms.internal = info.internal;
		terms.compressed = info.compressed;
		if (count >= TASK_VM_INFO_REV3_COUNT) {
			terms.graphics = positive(info.ledger_tag_graphics_footprint +
				info.ledger_tag_graphics_footprint_compressed);
			terms.media = positive(info.ledger_tag_media_footprint +
				info.ledger_tag_media_footprint_compressed);
		}
	}
	return terms;
}

} // namespace uml::runtime
