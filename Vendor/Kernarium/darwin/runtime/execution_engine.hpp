// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace uml::runtime {

/* Host-side policy only. Linux selects no execution engine through the fixed
 * C bridge; public handles and internal tests select it through Context.
 * These values are reported unchanged as uml_emulator_execution_model, so a
 * retired selection keeps its number rather than renumbering the others. */
enum class ExecutionEngine : std::uint8_t {
	automatic = 0,
	gadgets = 1,
	jit = 2,
};

/* The maintained kernel clamps ncpus= to CONFIG_NR_CPUS=64. Keep the host's
 * cache-budget divisor on that same effective range.
 */
inline constexpr unsigned maximum_guest_cpu_count = 64;

[[nodiscard]] bool execution_engine_available(
	ExecutionEngine engine) noexcept;

/* Publishes the whole-process budget for translated-code caches and the
 * private per-executor cap. The first valid configuration owns process-wide
 * geometry; identical repeats succeed and incompatible later runs fail.
 * Composition roots reach the CPU layer only through here.
 */
[[nodiscard]] bool set_translation_cache_budget(std::size_t total_bytes,
	unsigned executors, std::size_t per_executor_bytes) noexcept;

[[nodiscard]] bool acquire_translation_cache_run_session() noexcept;
void release_translation_cache_run_session() noexcept;

/* States the footprint a launch gives as the process's memory limit, for a
 * host that sets none (platform/host_environment.hpp); zero withdraws it. The
 * limit the budgets derive from, and the one a virtual Jetsam watches. Before
 * set_gadget_cache_budget.
 */
void set_process_memory_limit(std::size_t bytes) noexcept;

/* The gadget-cache budget a run of `guest_memory_bytes` of guest memory and
 * `executors` vCPUs takes when `bytes` is what a launch states (zero for none):
 * `bytes` when nonzero, otherwise a quarter of the guest's memory, at least 128
 * MiB, at most an eighth of the memory the host lets the process reach and,
 * where the host sets a limit, at most the cap of that many vCPUs
 * (runtime/guest_memory_budget.hpp, which also holds the rest of the memory the
 * guest side of the process uses). It publishes nothing, so a composition root
 * can add the budget to that memory before the run sets it.
 */
[[nodiscard]] std::size_t gadget_cache_budget_for(
	std::size_t guest_memory_bytes, std::size_t bytes,
	unsigned executors) noexcept;

/* Sets the process's byte budget for what the gadget engines' caches keep per
 * address space (cpu/gadget_cache_budget.hpp) and returns it: the figure
 * gadget_cache_budget_for gives. Before the run's first executor; a JIT run
 * sets it too, since an x86-64 address space starts on the gadget engine.
 */
std::size_t set_gadget_cache_budget(std::size_t guest_memory_bytes,
	std::size_t bytes, unsigned executors) noexcept;

/* Parses a byte count with an optional K/M/G suffix. Zero if unparsable. */
[[nodiscard]] std::size_t parse_translation_cache_quantity(
	const char *text) noexcept;

/* The maintained Darwin kernel selects the online host processor count when
 * ncpus= is absent, capped by CONFIG_NR_CPUS. The emulator uses the same value
 * before constructing per-vCPU translation caches.
 */
[[nodiscard]] unsigned default_guest_cpu_count() noexcept;

} // namespace uml::runtime
