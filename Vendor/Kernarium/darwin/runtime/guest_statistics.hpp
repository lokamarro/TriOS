// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace uml::runtime {

/*
 * What the host can say about a running guest from the system's own
 * accounting, for an application that shows statistics. Nothing here runs
 * until it is called: there is no sampler, timer or counter of its own, and a
 * call asks the guest nothing. The system keeps every thread's CPU time
 * whether or not anyone reads it, and the commitment of the guest's RAM keeps
 * the count of pages the guest released for its own sake.
 */

/* Whether `name` is a vCPU carrier's, uml-vcpu-N-carrier with N in decimal.
 * The kernel names its host threads (kernel/common/README.md), and the
 * carriers are the only ones that run guest code. */
[[nodiscard]] bool is_vcpu_carrier_name(std::string_view name) noexcept;

struct GuestCpuTime final {
	/* The carriers found: the guest's vCPUs that have come up. */
	unsigned carriers{};
	/* The user and system time those threads have used, in nanoseconds. A
	 * carrier that has ended takes its time with it, so the sum of a guest
	 * that restarted starts again from zero. */
	std::uint64_t nanoseconds{};
	/* False when the process's threads could not be listed. */
	bool listed{};
};

/* The CPU time of the process's vCPU carriers. It lists the process's threads
 * and reads each one's name and times, about 40 to 90 microseconds for the
 * 40 to 90 threads of a running guest. Any thread. */
[[nodiscard]] GuestCpuTime guest_cpu_time() noexcept;

struct GuestRamUsage final {
	/* The guest's RAM, kernel image included. */
	std::uint64_t ram_bytes{};
	/* The part of it the host holds now: everything the guest has committed
	 * and not released, and the kernel image, which is never released. */
	std::uint64_t held_bytes{};
};

/* The guest's RAM and the part of it the host holds, or nothing before the
 * guest's kernel has claimed its RAM, while the guest is being handed back to
 * the process and after. Any thread; it never waits: it reads two counters
 * unless a GuestRamRetirementScope is held. */
[[nodiscard]] std::optional<GuestRamUsage> guest_ram_usage() noexcept;

/*
 * The parts of the process's footprint the memory budgets count
 * (guest_memory_budget.hpp), in bytes, for the session record's memory
 * checkpoint (session_record.hpp), so that a record of a process the system
 * ended says which part had grown. Every figure but malloc's is a counter
 * something keeps anyway.
 */
struct MemoryTerms final {
	/* guest_ram_usage()'s held bytes; 0 without a guest. */
	std::uint64_t guest_held{};
	/* The host GPU's budget B and what the GPU held against it when its
	 * device last counted, and how long ago that was (-1: never); all 0
	 * without a budget (platform/gpu_memory_figures.hpp). */
	std::uint64_t gpu_budget{};
	std::uint64_t gpu_held{};
	std::int64_t gpu_age_ms{-1};
	/* The footprint's graphics and media ledgers, compressed pages
	 * included. */
	std::uint64_t graphics{};
	std::uint64_t media{};
	/* What the gadget caches hold, the accounts asked to shed included
	 * (cpu/gadget_cache_budget.hpp). */
	std::uint64_t cache{};
	/* The engine's domain heaps, committed (memory/domain_heap.hpp). */
	std::uint64_t heaps{};
	/* Every malloc zone's bytes, allocated to the zone and in use; 0 unless
	 * asked for. */
	std::uint64_t malloc_allocated{};
	std::uint64_t malloc_in_use{};
	/* The footprint's anonymous memory and its compressed share. */
	std::uint64_t internal{};
	std::uint64_t compressed{};
};

/* The terms now; any thread, and it never waits on the guest. With
 * `read_malloc`, also the malloc zones' statistics: a walk of the zones that
 * holds their locks while it runs, so every thread that allocates or frees
 * meanwhile waits for it, about 4 ms with 900 MiB of malloc. The caller must
 * hold no zone's lock. */
[[nodiscard]] MemoryTerms memory_terms(bool read_malloc) noexcept;

/*
 * Held by whoever retires the guest's RAM commitment (the hand-back of a
 * guest that restarted), so that a reader of guest_ram_usage() never meets
 * the object while it is deleted: a reader that finds it held reports
 * nothing. The commitment is deleted once per restart, never while a guest
 * runs; this exists for the reader that arrives between a guest's end and
 * the next one's start.
 */
class GuestRamRetirementScope final {
public:
	GuestRamRetirementScope() noexcept;
	~GuestRamRetirementScope();

	GuestRamRetirementScope(const GuestRamRetirementScope &) = delete;
	GuestRamRetirementScope &operator=(
		const GuestRamRetirementScope &) = delete;
};

} // namespace uml::runtime
