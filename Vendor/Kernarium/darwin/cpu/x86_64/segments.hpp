// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/decoder.hpp"
#include "memory/guest_address_space.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

/* x86 segmentation as Linux user code sees it on an x86-64 kernel, which both
 * engines share: the GDT Linux gives every task, its TLS entries and LDT, the
 * descriptor cache of each segment register, the selector loads, far
 * transfers and IRETs that change them, and the instructions that inspect
 * them. Neither engine implements any of these instructions itself; each
 * hands them, unretired, to `execute_segment_instruction`, which completes
 * them or raises their fault. */
namespace uml::cpu::x86_64 {

enum class SegmentRegister : std::uint8_t {
	es,
	cs,
	ss,
	ds,
	fs,
	gs,
};

inline constexpr std::uint8_t segment_not_present_vector = 11;
inline constexpr std::uint8_t stack_segment_vector = 12;
inline constexpr std::uint8_t general_protection_vector = 13;

/* A #NP, #SS or #GP and its error code; a vector of zero is none. */
struct SegmentFault final {
	std::uint8_t vector{};
	std::uint32_t error{};

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return vector != 0;
	}
};

/* The guest memory a segment operation reads and writes, through the engine
 * running it: a failed access leaves the engine's record of the fault, and a
 * store to code publishes as the engine's own stores do. Addresses are
 * linear. */
class SegmentMemory {
public:
	[[nodiscard]] virtual bool read(std::uint64_t address,
		std::span<std::byte> destination) noexcept = 0;
	[[nodiscard]] virtual bool write(std::uint64_t address,
		std::span<const std::byte> source) noexcept = 0;

protected:
	~SegmentMemory() = default;
};

/* At every entry from the guest kernel the descriptor caches are loaded
 * again from the selectors, the TLS descriptors and the LDT, as Linux's
 * return to user mode reloads the segment registers: CS, then SS, must name a
 * segment IRET would load, or the entry raises that fault before any
 * instruction runs; a DS, ES, FS or GS that no longer names a usable segment
 * becomes null, as Linux's reload of one does. The execution mode follows CS,
 * and FS's and GS's bases stay what the kernel saved. */
[[nodiscard]] SegmentFault enter_segments(CpuState &cpu) noexcept;

/* Recomputes the inline checks of the descriptor caches and
 * `flat_segments` after a change to any of them. */
void refresh_segment_checks(CpuState &cpu) noexcept;

/* Whether the engines leave `instruction` to execute_segment_instruction:
 * a selector load (MOV or POP to a segment register, LDS, LES, LSS, LFS,
 * LGS), a far CALL, JMP or RET, IRET, and LAR, LSL, VERR, VERW, SLDT and
 * STR. */
[[nodiscard]] bool segment_instruction(
	const Instruction &instruction) noexcept;

enum class SegmentOutcome : std::uint8_t {
	/* The instruction retired; RIP names the next one. */
	retired,
	/* It retired and changed the execution mode or `flat_segments`, so
	 * code built for the old ones must not run on. */
	retired_changed,
	/* An access faulted, as the memory recorded; nothing committed. */
	memory_fault,
	/* `fault` names the #NP, #SS or #GP; nothing committed. */
	fault,
	/* Only from `prepare_segment_step`: every access the instruction makes
	 * passes, and the engine may run it, alone, under a SegmentBypass. */
	run,
	/* Only from `prepare_segment_step`: a repeated string instruction
	 * stopped between elements after a bounded amount of work, with RF set
	 * as an interrupt would leave it. It has not retired; RIP names it. */
	yielded,
};

struct SegmentResult final {
	SegmentOutcome outcome{SegmentOutcome::fault};
	SegmentFault fault{};
	/* IRET set RFLAGS.TF: the next instruction raises #DB after it. */
	bool trap_armed{};
};

/* Runs `instruction`, which RIP names and segment_instruction accepts,
 * against the descriptor tables, as x86 does at CPL 3 with UMIP. */
[[nodiscard]] SegmentResult execute_segment_instruction(CpuState &cpu,
	SegmentMemory &memory, const Instruction &instruction) noexcept;

/* The exact check of every access a compatibility-mode `instruction`,
 * which RIP names, makes through a segment, for an instruction an engine
 * could not check inline: the fault the first failing access raises, or
 * `run`. String instructions, XLAT, and the stack instructions of a task
 * whose SS has a base or a limit, which the engines' address generation
 * cannot honour, run here instead, and retire; a repeated string
 * instruction yields after `string_elements` elements. */
[[nodiscard]] SegmentResult prepare_segment_step(CpuState &cpu,
	SegmentMemory &memory, const Instruction &instruction,
	std::uint64_t string_elements) noexcept;

/* While it lives, every inline check passes and the guard of an instruction
 * the engines cannot check lets it run: the runtime has checked the one
 * instruction it now runs. */
class SegmentBypass final {
public:
	explicit SegmentBypass(CpuState &cpu) noexcept;
	~SegmentBypass();
	SegmentBypass(const SegmentBypass &) = delete;
	SegmentBypass &operator=(const SegmentBypass &) = delete;

private:
	CpuState &cpu_;
};

/* The segment model's accesses through a guest address space, for a runtime
 * that runs it between dispatches: a failed one keeps its fault, and a store
 * to an executable page publishes the change, which each engine's next
 * preparation retires its code for. */
class AddressSpaceSegmentMemory final : public SegmentMemory {
public:
	AddressSpaceSegmentMemory(::uml::memory::GuestAddressSpace &memory,
		void *execution_context) noexcept :
		memory_(memory), execution_context_(execution_context)
	{
	}

	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> destination) noexcept override;
	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> source) noexcept override;
	[[nodiscard]] ::uml::cpu::FaultInfo fault() const noexcept
	{
		return fault_;
	}

private:
	void record(std::uint64_t address,
		::uml::memory::MemoryAccess access) noexcept;

	::uml::memory::GuestAddressSpace &memory_;
	void *execution_context_{};
	::uml::cpu::FaultInfo fault_{};
};

/* What the segment model made of the instruction RIP names, run between an
 * engine's dispatches. */
struct SegmentStep final {
	SegmentResult result{};
	/* For `run`: the instruction the engine is to run alone, under a
	 * SegmentBypass, with each memory operand marked to add its segment's
	 * base in a task whose ES, SS or DS has one. */
	Instruction instruction{};
	/* A fault fetching the instruction, or in the model's own accesses. */
	::uml::cpu::FaultInfo fault{};
	bool fetch_fault{};
	bool illegal{};
};

/* prepare_segment_step for the instruction RIP names. */
[[nodiscard]] SegmentStep check_segment_step(CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, void *execution_context,
	std::uint64_t string_elements) noexcept;

/* execute_segment_instruction for the instruction RIP names, which an engine
 * left to the segment model. */
[[nodiscard]] SegmentStep run_segment_step(CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory,
	void *execution_context) noexcept;

/* Whether a compatibility-mode instruction reaches FS or GS in a way the
 * engines' inline check does not cover -- a string instruction, XLAT, a
 * gather, a masked store, or an operand larger than the check's slack --
 * or names CS, through which a store faults: the engines guard it, and it
 * runs only after the runtime's exact check. */
[[nodiscard]] bool segment_guarded(const Instruction &instruction) noexcept;

/* LSL: the limit of the segment `selector` names, as x86 reads it at CPL
 * 3, or nothing where it would clear ZF. */
[[nodiscard]] std::optional<std::uint32_t> segment_limit(
	const CpuState &cpu, std::uint16_t selector) noexcept;

/* The segment register a memory operand of `instruction` reaches through:
 * its override, or SS for an address based on RSP or RBP and DS for any
 * other. Long mode adds no base but FS's and GS's. */
[[nodiscard]] SegmentRegister operand_segment(
	const MemoryOperand &memory) noexcept;

/* The x86-64 kernel's TLS descriptor for a struct user_desc as the bridge
 * carries it: its entry number and base, then its limit and flags. */
[[nodiscard]] std::uint64_t tls_descriptor(std::uint64_t head,
	std::uint64_t tail) noexcept;

} // namespace uml::cpu::x86_64
