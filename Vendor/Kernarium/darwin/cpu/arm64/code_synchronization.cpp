// SPDX-License-Identifier: GPL-2.0-or-later
#include "code_synchronization.hpp"

#include "uml_darwin_static_mmu.h"

#include <atomic>

extern "C" [[gnu::noinline]] void
uml_arm64_guest_code_synchronize(std::uint64_t address) noexcept
{
	/* Guest data and instruction views share one physical backing, but
	 * translated programs still need an explicit sequence change at the
	 * architectural synchronization point. */
	std::atomic_thread_fence(std::memory_order_seq_cst);
	darwin_uml_static_vm_invalidate_code(
		static_cast<unsigned long>(address));
}
