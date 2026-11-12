// SPDX-License-Identifier: GPL-2.0-or-later
//
// The library-exit counters (semantics_exit_counters.hpp), compiled into the
// semantic library's unit-test variant only.

#include "cpu/arm64/aarch32/semantics_exit_counters.hpp"

#include <array>
#include <atomic>

namespace uml::cpu::arm64::aarch32 {

namespace {

std::atomic<bool> counting_exits{false};
std::atomic<std::uint64_t> exit_total{0};
std::array<std::atomic<std::uint64_t>, kFormCount> form_exits{};

}

void detail::note_library_exit(Form form) noexcept
{
	if (counting_exits.load(std::memory_order_relaxed)) {
		form_exits[static_cast<std::size_t>(form)].fetch_add(
			1, std::memory_order_relaxed);
		exit_total.fetch_add(1, std::memory_order_relaxed);
	}
}

void count_a32_library_exits(bool enable) noexcept
{
	counting_exits.store(enable, std::memory_order_relaxed);
}

std::uint64_t a32_library_exits(Form form) noexcept
{
	return form_exits[static_cast<std::size_t>(form)].load(
		std::memory_order_relaxed);
}

std::uint64_t a32_library_exit_total() noexcept
{
	return exit_total.load(std::memory_order_relaxed);
}

void reset_a32_library_exits() noexcept
{
	exit_total.store(0, std::memory_order_relaxed);
	for (auto &count : form_exits)
		count.store(0, std::memory_order_relaxed);
}

} // namespace uml::cpu::arm64::aarch32
