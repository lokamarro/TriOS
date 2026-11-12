// SPDX-License-Identifier: GPL-2.0-or-later
//
// The C++ half of the AArch32 gadget part's library exit (engine/a32.inc,
// a32_library_call): the a32_library gadget, and the slow paths of the block
// transfers, run one instruction through the shared semantic library
// (cpu/arm64/aarch32/semantics.hpp) on CpuState. The gadget has stored the
// guest's NZCV in CpuState first and reloads it after. The library decodes
// the instruction again from the word and ITSTATE the stream carries, in the
// profile the builder decoded it in, and executes it with the architecture's
// fault ordering; guest memory is the executor's TLB.
#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/semantics.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/gadgets/a32_layout.h"
#include "memory/tlb.hpp"

#include <cstdint>

namespace uml::cpu::arm64::gadgets {
namespace {

namespace a32 = ::uml::cpu::arm64::aarch32;

/* Guest memory through the executor's TLB. The library forms 32-bit
 * addresses, so the plain translation applies. A store gives no notice
 * beyond the TLB's own write protection of translated code, as a gadget
 * store gives none. */
class GadgetMemory final : public a32::A32Memory {
public:
	explicit GadgetMemory(memory::Tlb &tlb) noexcept : tlb_{tlb} {}

	std::byte *translate(std::uint64_t address,
		memory::MemoryAccess access) noexcept override
	{
		return tlb_.translate(address, memory::translated_access(access));
	}

	void stored(std::byte *, std::size_t) noexcept override {}

private:
	memory::Tlb &tlb_;
};

} // namespace
} // namespace uml::cpu::arm64::gadgets

/* Runs the instruction at `key` that `operand` names (a32_layout.h) and
 * returns an UML_GADGET_A32_LIBRARY_* outcome; a memory fault is in the TLB.
 * The PC and ITSTATE of CpuState are the library's scratch here: the
 * translated code keeps neither. A block transfer passes its position
 * (a32_layout.h) in place of the key and reads no PC, so it runs at address
 * 0 of the instruction set the operand names. */
extern "C" [[gnu::noinline]] std::uint32_t uml_arm64_gadget_a32_execute(
	uml::cpu::arm64::CpuState *cpu, uml::memory::Tlb *tlb,
	std::uint64_t operand, std::uint64_t key) noexcept
{
	namespace a32 = uml::cpu::arm64::aarch32;
	namespace gadgets = uml::cpu::arm64::gadgets;
	if (cpu == nullptr || tlb == nullptr)
		return UML_GADGET_A32_LIBRARY_FATAL;
	const auto word = static_cast<std::uint32_t>(operand);
	const a32::ItState it{static_cast<std::uint8_t>(
		operand >> UML_GADGET_A32_LIBRARY_IT_SHIFT)};
	const auto form = static_cast<a32::Form>(
		operand >> UML_GADGET_A32_LIBRARY_FORM_SHIFT);
	/* The builder emitted this gadget for a word it decoded in the engine's
	 * profile (a32::decode_profile), so the decode here only recovers
	 * what it found and needs no host knowledge. */
	const a32::Profile profile;
	const bool thumb =
		(operand >> UML_GADGET_A32_LIBRARY_THUMB_SHIFT & 1U) != 0U;
	const auto instruction = thumb ? a32::decode_thumb(word, it, profile) :
					 a32::decode_a32(word, profile);
	if (instruction.form != form)
		return UML_GADGET_A32_LIBRARY_FATAL;
	const auto saved_pc = cpu->pc;
	const auto saved_it = cpu->a32_itstate;
	cpu->pc = (key & 3U) != 1U ? key : thumb ? a32::t32_key(0) :
						    a32::a32_key(0);
	cpu->a32_itstate = it.bits();
	gadgets::GadgetMemory memory{*tlb};
	const auto result = a32::execute_a32(*cpu, instruction, memory);
	const auto target = cpu->pc;
	cpu->pc = saved_pc;
	cpu->a32_itstate = saved_it;
	switch (result.event) {
	case a32::StepEvent::retired:
		if ((operand >> UML_GADGET_A32_LIBRARY_PC_VALUE_SHIFT & 1U) != 0)
			cpu->registers[15] = a32::key_target_value(target);
		return UML_GADGET_A32_LIBRARY_RETIRED;
	case a32::StepEvent::memory_fault:
		tlb->fault_address = result.fault_address;
		tlb->fault_access = uml::memory::translated_access(result.access);
		tlb->fault_alignment = result.alignment ? 1U : 0U;
		return UML_GADGET_A32_LIBRARY_FAULT;
	case a32::StepEvent::undefined:
		return UML_GADGET_A32_LIBRARY_UNDEFINED;
	case a32::StepEvent::supervisor_call:
	case a32::StepEvent::breakpoint:
	case a32::StepEvent::pc_alignment:
	case a32::StepEvent::fatal:
		break;
	}
	return UML_GADGET_A32_LIBRARY_FATAL;
}
