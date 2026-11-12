// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "kernel_resources.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::runtime {

/*
 * Handing the process's guest back.
 *
 * A guest that restarts itself ends its run normally and leaves the process
 * to the next boot. What the kernel took from the process and did not give
 * back is the library's to take back before that boot, and this is the part of
 * the hand-back that is the process's rather than the handle's:
 *
 *   - the kernel's descriptors, pthread keys, allocations and mappings
 *     (kernel_resources.hpp), closed, deleted, freed and unmapped through the
 *     ledger, after the run's memory domain was retired so that retirement
 *     never meets a closed descriptor, and while no kernel thread runs;
 *   - the records of the guest's writable host files (platform/guest_files),
 *     which name descriptor numbers the next guest may be given for something
 *     else; and
 *   - the guest's RAM: the whole native window becomes one protectionless
 *     reservation again, the commitment is retired and the token is
 *     unregistered, so the next kernel registers its own and finds no byte of
 *     this one (memory/shared_backing.hpp).
 *
 * The engine's own metadata for the run, the MMU's indexes and the gadget
 * caches, is in heaps the memory layer gave back when the run's memory domain
 * retired, which the hand-back follows (memory/domain_heap.hpp). The hand-back
 * reports what those heaps still hold, which is nothing of the run's but the
 * scratch chunk each thread that mapped pages keeps for its life.
 *
 * The handle's own state, which describes this kernel and its RAM, is reset
 * between the check and the hand-back, before the commitment goes. The caller
 * holds the process address-space gate: only one run uses the window at a
 * time.
 */

enum class GuestHandBackBlock : std::uint8_t {
	/* Nothing stands in the way. */
	none,
	/* A host service still borrows guest RAM: the caller stops its device
	 * services first. */
	borrowed_ram,
	/* A kernel thread still runs, or a fence still reads the thread table. */
	kernel_threads,
};

/* Whether the guest can be handed back now. Changes nothing but the slots of
 * threads that have ended, so a refusal leaves the guest as it was. */
[[nodiscard]] GuestHandBackBlock guest_hand_back_block() noexcept;

struct GuestHandBack final {
	/* What the ledger gave back. */
	KernelResourceCounts released{};
	/* The guest-file records forgotten. */
	std::size_t guest_files{};
	/* The domain heaps alive, the bytes of the heap region committed (the
	 * threads' kept scratch included) and the leases on the engine's shared
	 * heap, after the hand-back. */
	std::size_t heaps{};
	std::size_t heap_bytes{};
	std::size_t heap_leases{};
};

/* Hands the guest back and describes what came back. False, with errno set
 * by the failing step, when the window could not be replaced: the guest is
 * then still registered and the call may be repeated; the steps before it are
 * idempotent. */
[[nodiscard]] bool hand_back_guest(GuestHandBack &hand_back) noexcept;

} // namespace uml::runtime
