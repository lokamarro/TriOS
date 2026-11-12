// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/tlb.hpp"
#include "cpu/arm64/layout/assembly_layout.hpp"

#include <cstddef>
#include <cstdint>

static_assert(sizeof(::uml::memory::TlbEntry) == 32);
static_assert(alignof(::uml::memory::TlbEntry) == 32);
static_assert(offsetof(::uml::memory::Tlb, entries) == 0);
static_assert(offsetof(::uml::memory::Tlb, generation) ==
	::uml::memory::tlb_entry_count * sizeof(::uml::memory::TlbEntry));
static_assert(offsetof(::uml::memory::Tlb, changes) ==
	offsetof(::uml::memory::Tlb, generation) + sizeof(std::uint64_t));
static_assert(offsetof(::uml::memory::Tlb, fault_address) ==
	offsetof(::uml::memory::Tlb, changes) + sizeof(std::uint64_t));
static_assert(offsetof(::uml::memory::Tlb, fault_access) ==
	offsetof(::uml::memory::Tlb, fault_address) + sizeof(std::uint64_t));

int main()
{
	static_assert(uml::cpu::arm64::layout::version == 1);
	return 0;
}
