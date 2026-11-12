/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "platform/gpu_budget.hpp"
#include "runtime/guest_memory_budget.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uml::emulator {

/* The launch parameters of the host GPU's fixed budget
 * (platform/gpu_budget.hpp): A, the application's expected ceiling; E_rest,
 * the guest side's other memory beyond the gadget cache and the slot tags; and
 * S, the headroom. Each defaults to the conservative launch set
 * (runtime/guest_memory_budget.hpp), the one the default guest RAM is measured
 * with. */
struct GpuBudgetParameters final {
	std::uint64_t application{
		::uml::runtime::guest_memory::launch_memory_set.application};
	std::uint64_t engine_rest{
		::uml::runtime::guest_memory::launch_memory_set.engine_rest};
	std::uint64_t headroom{
		::uml::runtime::guest_memory::launch_memory_set.headroom};

	[[nodiscard]] bool operator==(const GpuBudgetParameters &) const = default;
};

/* The budget of one launch, with the figures it derives from. */
struct GpuBudgetPlan final {
	platform::GpuBudget budget;
	/* L: the process's memory limit, zero for none. */
	std::uint64_t process_limit{};
	/* G: the guest's RAM. */
	std::uint64_t guest_memory{};
	/* v: the guest's vCPUs. */
	unsigned guest_cpus{1};
	/* E: the gadget-cache budget the run will set
	 * (runtime::set_gadget_cache_budget), the slot tags of the vCPUs and
	 * E_rest (runtime/guest_memory_budget.hpp). */
	std::uint64_t engine{};
	GpuBudgetParameters parameters;
};

/* Evaluates the budget for a guest of `guest_memory` bytes and `guest_cpus`
 * vCPUs whose launch states `memory_limit_option` as the process's limit
 * (--memory-limit=, zero for none) and `gadget_cache_option` as the gadget
 * cache's budget (--gadget-cache-budget=, zero to derive it). The limit option
 * is published as the run will publish it, so the plan sees the limit the run
 * will.
 *
 * Both composition roots use this one function, before the GPU service is
 * created, so the macOS launcher and the iOS runtime cannot disagree. */
[[nodiscard]] GpuBudgetPlan plan_gpu_budget(std::uint64_t guest_memory,
	std::size_t memory_limit_option, std::size_t gadget_cache_option,
	unsigned guest_cpus, const GpuBudgetParameters &parameters);

/* The same, reading mem=, ncpus=, --memory-limit= and --gadget-cache-budget=
 * from the arguments the emulator is about to be given. */
[[nodiscard]] GpuBudgetPlan plan_gpu_budget(
	const std::vector<std::string> &arguments,
	const GpuBudgetParameters &parameters);

/* The byte count the last `option` ("--x=") argument states as the emulator
 * reads it (a number with an optional K, M or G suffix), or zero when it is
 * absent or unreadable. */
[[nodiscard]] std::size_t configured_option_bytes(
	const std::vector<std::string> &arguments, std::string_view option);

/* The mem= argument a launcher gives a launch that gives none: `fallback`, the
 * launcher's own size, or, where the launch states the process's memory limit
 * (--memory-limit=, else UML_MEMORY_LIMIT_MIB), the default guest RAM for that
 * limit at the launch's vCPU count (ncpus=, else the applications' default;
 * runtime/guest_memory_budget.hpp), so a Mac run sizes its guest as the
 * device does. */
[[nodiscard]] std::string default_guest_memory_argument(
	const std::vector<std::string> &arguments, std::string_view fallback);

/* A launch option's byte count for the host GPU budget: a whole number with
 * an optional K, M or G suffix, where zero is a value too. False when `text`
 * is anything else. */
[[nodiscard]] bool parse_gpu_budget_bytes(
	std::string_view text, std::uint64_t &bytes) noexcept;

/* The one line a launch prints when the process has less than the floor left
 * for the GPU (GpuBudget::short_of_floor), without a trailing newline. */
[[nodiscard]] std::string describe_gpu_budget_shortfall(
	const GpuBudgetPlan &plan);

} // namespace uml::emulator
