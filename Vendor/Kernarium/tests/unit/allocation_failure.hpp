// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

namespace uml::testing {

enum class AllocationForm {
	scalar,
	array,
	scalar_nothrow,
	array_nothrow,
	aligned_scalar,
	aligned_array,
	aligned_scalar_nothrow,
	aligned_array_nothrow,
};

struct AllocationFailure final {
	std::size_t size{};
	std::size_t alignment{};
	AllocationForm form{AllocationForm::scalar};
	bool observed{};
};

/* Fail the next allocation performed by the calling host thread.  The test
 * executable supplies the replacement allocation functions; production
 * objects contain no fault-injection field, symbol, or conditional branch. */
void fail_next_allocation() noexcept;
/* Permit this many matching-thread allocations to succeed, then fail the
 * following allocation. Zero is equivalent to fail_next_allocation(). */
void fail_after_allocations(std::size_t successful_allocations) noexcept;
[[nodiscard]] bool cancel_pending_allocation_failure() noexcept;
[[nodiscard]] AllocationFailure last_allocation_failure() noexcept;

/* Count allocations on the calling thread until the matching end call. */
void begin_allocation_count() noexcept;
[[nodiscard]] std::size_t end_allocation_count() noexcept;

} // namespace uml::testing
