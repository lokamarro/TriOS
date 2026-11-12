// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlb.hpp"

#include "../cpu_state.hpp"
#include "../detail/memory_copy_set.hpp"

#include "uml_darwin_static_mmu.h"
#include "uml_darwin_guest.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace uml::memory {
namespace {

using ::uml::cpu::arm64::CpuState;

[[nodiscard]] std::uint64_t &cpu_slot(
	CpuState *cpu, std::uint16_t offset) noexcept
{
	return *reinterpret_cast<std::uint64_t *>(
		reinterpret_cast<std::byte *>(cpu) + offset);
}

using ::uml::cpu::arm64::detail::MopsFamily;
using ::uml::cpu::arm64::detail::MopsOutcome;

template <MopsFamily family>
void mops_prologue(CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto destination_offset = static_cast<std::uint16_t>(operand);
	const auto count_offset = static_cast<std::uint16_t>(operand >> 16);
	const auto source_offset = static_cast<std::uint16_t>(operand >> 32);
	::uml::cpu::arm64::detail::mops_prologue<family>(*cpu,
		cpu_slot(cpu, destination_offset), cpu_slot(cpu, count_offset),
		cpu_slot(cpu, source_offset));
}

/* Returns 1 on completion, 0 for a fault with the registers committed at the
 * last chunk boundary, and 2 for an epilogue the prologue cannot have
 * formatted. */
template <MopsFamily family, bool source_zero = false>
[[nodiscard]] std::uint32_t mops_epilogue(CpuState *cpu, Tlb *tlb,
	std::uintptr_t operand) noexcept
{
	const auto destination_offset = static_cast<std::uint16_t>(operand);
	const auto count_offset = static_cast<std::uint16_t>(operand >> 16);
	const auto source_offset = static_cast<std::uint16_t>(operand >> 32);
	std::uint64_t zero{};
	auto &source = source_zero ? zero : cpu_slot(cpu, source_offset);
	switch (::uml::cpu::arm64::detail::mops_epilogue<family>(*cpu, *tlb,
		cpu_slot(cpu, destination_offset), cpu_slot(cpu, count_offset),
		source)) {
	case MopsOutcome::complete:
		return 1;
	case MopsOutcome::fault:
		return 0;
	case MopsOutcome::illegal:
		break;
	}
	return 2;
}

} // namespace

} // namespace uml::memory

#define UML_MOPS_PROLOGUE_WRAPPER(name, operation) \
extern "C" void uml_arm64_gadget_mops_prologue_transfer_##name( \
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept \
{ \
	using namespace uml::memory; \
	mops_prologue<MopsFamily::operation>(cpu, operand); \
}

#define UML_MOPS_EPILOGUE_WRAPPER(name, operation, source_zero) \
extern "C" std::uint32_t uml_arm64_gadget_mops_epilogue_transfer_##name( \
	uml::cpu::arm64::CpuState *cpu, \
	::uml::memory::Tlb *tlb, \
	std::uintptr_t operand) noexcept \
{ \
	using namespace uml::memory; \
	return mops_epilogue<MopsFamily::operation, source_zero>( \
		cpu, tlb, operand); \
}

UML_MOPS_PROLOGUE_WRAPPER(copy_forward, copy_forward)
UML_MOPS_PROLOGUE_WRAPPER(copy_general, copy_general)
UML_MOPS_PROLOGUE_WRAPPER(set, set)
UML_MOPS_EPILOGUE_WRAPPER(copy_forward, copy_forward, false)
UML_MOPS_EPILOGUE_WRAPPER(copy_general, copy_general, false)
UML_MOPS_EPILOGUE_WRAPPER(set_r, set, false)
UML_MOPS_EPILOGUE_WRAPPER(set_z, set, true)

#undef UML_MOPS_PROLOGUE_WRAPPER
#undef UML_MOPS_EPILOGUE_WRAPPER

