// SPDX-License-Identifier: GPL-2.0-or-later
#include "allocation_fault.hpp"

#if !UML_MEMORY_ALLOCATION_TESTING
#error "allocation_fault.cpp is test-variant support only"
#endif

#include <array>
#include <atomic>
#include <new>

namespace uml::memory::testing {
namespace {

std::atomic<AllocationSite> failure_site{};
std::atomic_size_t failure_occurrence{};
std::atomic_size_t backing_mappings{};
std::atomic<AllocationObserver> allocation_observer{};
std::array<std::atomic_size_t,
	static_cast<std::size_t>(AllocationSite::count)> allocation_attempts{};

} // namespace

void fail_next_allocation(const AllocationSite site) noexcept
{
	fail_allocation_at(site, 1);
}

void fail_allocation_at(const AllocationSite site,
	const std::size_t occurrence) noexcept
{
	failure_occurrence.store(occurrence == 0 ? 1 : occurrence,
		std::memory_order_release);
	failure_site.store(site, std::memory_order_release);
}

void clear_allocation_failure() noexcept
{
	failure_site.store(AllocationSite::none, std::memory_order_release);
	failure_occurrence.store(0, std::memory_order_release);
}

void set_allocation_observer(const AllocationObserver observer) noexcept
{
	allocation_observer.store(observer, std::memory_order_release);
}

void allocation_checkpoint(const AllocationSite site)
{
	if (const auto observer =
		    allocation_observer.load(std::memory_order_acquire))
		observer(site);
	const auto site_index = static_cast<std::size_t>(site);
	if (site_index < allocation_attempts.size())
		allocation_attempts[site_index].fetch_add(1, std::memory_order_relaxed);
	if (failure_site.load(std::memory_order_acquire) != site)
		return;
	auto remaining = failure_occurrence.load(std::memory_order_acquire);
	while (remaining != 0) {
		if (!failure_occurrence.compare_exchange_weak(remaining, remaining - 1,
				std::memory_order_acq_rel, std::memory_order_acquire))
			continue;
		if (remaining != 1)
			return;
		auto expected = site;
		(void)failure_site.compare_exchange_strong(expected,
			AllocationSite::none, std::memory_order_acq_rel,
			std::memory_order_acquire);
		throw std::bad_alloc{};
	}
}

void reset_allocation_attempt_counts() noexcept
{
	for (auto &attempts : allocation_attempts)
		attempts.store(0, std::memory_order_relaxed);
}

std::size_t allocation_attempt_count(const AllocationSite site) noexcept
{
	const auto site_index = static_cast<std::size_t>(site);
	return site_index < allocation_attempts.size() ?
		allocation_attempts[site_index].load(std::memory_order_relaxed) : 0;
}

void backing_mapping_created() noexcept
{
	backing_mappings.fetch_add(1, std::memory_order_relaxed);
}

void backing_mapping_destroyed() noexcept
{
	backing_mappings.fetch_sub(1, std::memory_order_relaxed);
}

std::size_t active_backing_mappings() noexcept
{
	return backing_mappings.load(std::memory_order_relaxed);
}

} // namespace uml::memory::testing
