// SPDX-License-Identifier: GPL-2.0-or-later
#include "allocation_failure.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

thread_local bool fail_next;
thread_local std::size_t successful_allocations_before_failure;
thread_local bool count_allocations;
thread_local std::size_t allocation_count;
thread_local uml::testing::AllocationFailure last_failure;

[[nodiscard]] bool consume_failure(std::size_t size,
	uml::testing::AllocationForm form,
	std::size_t alignment = 0) noexcept
{
	if (!fail_next)
		return false;
	if (successful_allocations_before_failure != 0) {
		--successful_allocations_before_failure;
		return false;
	}
	fail_next = false;
	last_failure = {size, alignment, form, true};
	return true;
}

[[nodiscard]] void *allocate(
	std::size_t size, uml::testing::AllocationForm form)
{
	if (consume_failure(size, form))
		throw std::bad_alloc{};
	if (count_allocations)
		++allocation_count;
	if (size == 0)
		size = 1;
	if (void *memory = std::malloc(size); memory != nullptr)
		return memory;
	throw std::bad_alloc{};
}

[[nodiscard]] void *allocate_aligned(
	std::size_t size, std::size_t alignment,
	uml::testing::AllocationForm form)
{
	if (consume_failure(size, form, alignment))
		throw std::bad_alloc{};
	if (count_allocations)
		++allocation_count;
	if (size == 0)
		size = 1;
	void *memory = nullptr;
	if (posix_memalign(&memory, alignment, size) == 0)
		return memory;
	throw std::bad_alloc{};
}

} // namespace

namespace uml::testing {

void fail_next_allocation() noexcept
{
	fail_after_allocations(0);
}

void fail_after_allocations(std::size_t successful_allocations) noexcept
{
	last_failure = {};
	successful_allocations_before_failure = successful_allocations;
	fail_next = true;
}

bool cancel_pending_allocation_failure() noexcept
{
	const bool pending = fail_next;
	fail_next = false;
	successful_allocations_before_failure = 0;
	return pending;
}

AllocationFailure last_allocation_failure() noexcept
{
	return last_failure;
}

void begin_allocation_count() noexcept
{
	allocation_count = 0;
	count_allocations = true;
}

std::size_t end_allocation_count() noexcept
{
	count_allocations = false;
	return allocation_count;
}

} // namespace uml::testing

void *operator new(std::size_t size)
{
	return allocate(size, uml::testing::AllocationForm::scalar);
}

void *operator new[](std::size_t size)
{
	return allocate(size, uml::testing::AllocationForm::array);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
	try {
		return allocate(size,
			uml::testing::AllocationForm::scalar_nothrow);
	} catch (...) {
		return nullptr;
	}
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
	try {
		return allocate(size,
			uml::testing::AllocationForm::array_nothrow);
	} catch (...) {
		return nullptr;
	}
}

void *operator new(std::size_t size, std::align_val_t alignment)
{
	return allocate_aligned(size, static_cast<std::size_t>(alignment),
		uml::testing::AllocationForm::aligned_scalar);
}

void *operator new[](
	std::size_t size, std::align_val_t alignment)
{
	return allocate_aligned(size, static_cast<std::size_t>(alignment),
		uml::testing::AllocationForm::aligned_array);
}

void *operator new(std::size_t size, std::align_val_t alignment,
	const std::nothrow_t &) noexcept
{
	try {
		return allocate_aligned(
			size, static_cast<std::size_t>(alignment),
			uml::testing::AllocationForm::aligned_scalar_nothrow);
	} catch (...) {
		return nullptr;
	}
}

void *operator new[](std::size_t size, std::align_val_t alignment,
	const std::nothrow_t &) noexcept
{
	try {
		return allocate_aligned(
			size, static_cast<std::size_t>(alignment),
			uml::testing::AllocationForm::aligned_array_nothrow);
	} catch (...) {
		return nullptr;
	}
}

void operator delete(void *memory) noexcept
{
	std::free(memory);
}

void operator delete[](void *memory) noexcept
{
	std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept
{
	std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept
{
	std::free(memory);
}

void operator delete(void *memory, const std::nothrow_t &) noexcept
{
	std::free(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept
{
	std::free(memory);
}

void operator delete(void *memory, std::align_val_t) noexcept
{
	std::free(memory);
}

void operator delete[](void *memory, std::align_val_t) noexcept
{
	std::free(memory);
}

void operator delete(void *memory, std::size_t, std::align_val_t) noexcept
{
	std::free(memory);
}

void operator delete[](void *memory, std::size_t, std::align_val_t) noexcept
{
	std::free(memory);
}

void operator delete(
	void *memory, std::align_val_t, const std::nothrow_t &) noexcept
{
	std::free(memory);
}

void operator delete[](
	void *memory, std::align_val_t, const std::nothrow_t &) noexcept
{
	std::free(memory);
}
