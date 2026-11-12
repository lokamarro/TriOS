// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/software_tlb.hpp"
#include "memory/guest_address_space.hpp"
#include "cpu/x86_64/layout/assembly_offsets.h"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::x86_64 {
class CodePageRegistry;
}

namespace uml::cpu::x86_64::gadgets {

struct IndirectLink;
struct TargetLinkEntry;

struct NativeExecutionContext final {
	::uml::memory::GuestAddressSpace *memory{};
	NativeTlbEntry *tlb_entries{};
	std::uint64_t program_begin{};
	std::uint64_t program_end{};
	::uml::cpu::FaultInfo fault{};
	std::uint8_t write_code{};
	/* This run holds the lock that serializes locked operands no exclusive
	 * access can cover; the fault tail releases it. */
	std::uint8_t split_lock{};
	std::size_t instruction_limit{};
	IndirectLink *indirect_miss{};
	const TargetLinkEntry *target_links{};
	void *execution_context{};
	void *sequence_context{};
	std::uint64_t (*code_change_reader)(void *) noexcept{};
	std::uint64_t code_changes{};
	std::uint64_t code_publications{};
	/* Pages holding a cached translation in any executor of this address
	 * space; null publishes on every executable-page store. */
	CodePageRegistry *code_pages{};
	/* The TLB entry the last stack access translated through. A stack access
	 * re-checks this entry's tag before hashing its address, and a plain-page
	 * tag lets it skip the store epilogue; it is never null. */
	NativeTlbEntry *stack_entry{};
	/* The stack pointer entry left the frame at, to which an exit that a
	 * gadget takes from inside its own frame returns. */
	std::uintptr_t entry_stack{};
	/* An exception-check exit's stream pointer, just past a stream word of
	 * the instruction that raised the exception. */
	const std::uintptr_t *exception_stream{};
	/* FPSR as the SIMD floating-point instruction running now found it,
	 * which a gadget saves while the gadgets check SIMD exceptions and its
	 * exception-check exit puts back. */
	std::uint64_t simd_status{};
	/* A segment-fault exit's #NP, #SS or #GP and its error code. */
	std::uint8_t segment_vector{};
	std::uint32_t segment_error{};
};

static_assert(offsetof(NativeExecutionContext, tlb_entries) ==
	UML_X86_64_CONTEXT_TLB);
static_assert(offsetof(NativeExecutionContext, program_begin) ==
	UML_X86_64_CONTEXT_PROGRAM_BEGIN);
static_assert(offsetof(NativeExecutionContext, program_end) ==
	UML_X86_64_CONTEXT_PROGRAM_END);
static_assert(offsetof(NativeExecutionContext, write_code) ==
	UML_X86_64_CONTEXT_WRITE_CODE);
/* A store translation asks whether a publication is still owed by testing this
 * field's low bit, so only the pending state may carry it. */
static_assert((UML_X86_64_WRITE_CODE_PENDING & 1U) == 1U &&
	(UML_X86_64_WRITE_CODE_PUBLISHED & 1U) == 0U);
static_assert(offsetof(NativeExecutionContext, split_lock) ==
	UML_X86_64_CONTEXT_SPLIT_LOCK);
static_assert(offsetof(NativeExecutionContext, instruction_limit) ==
	UML_X86_64_CONTEXT_INSTRUCTION_LIMIT);
static_assert(offsetof(NativeExecutionContext, indirect_miss) ==
	UML_X86_64_CONTEXT_INDIRECT_MISS);
static_assert(offsetof(NativeExecutionContext, target_links) ==
	UML_X86_64_CONTEXT_TARGET_LINKS);
static_assert(offsetof(NativeExecutionContext, stack_entry) ==
	UML_X86_64_CONTEXT_STACK_ENTRY);
static_assert(offsetof(NativeExecutionContext, entry_stack) ==
	UML_X86_64_CONTEXT_ENTRY_STACK);
static_assert(offsetof(NativeExecutionContext, exception_stream) ==
	UML_X86_64_CONTEXT_EXCEPTION_STREAM);
static_assert(offsetof(NativeExecutionContext, simd_status) ==
	UML_X86_64_CONTEXT_SIMD_STATUS);

struct NativeEngineResult final {
	std::int32_t event;
	std::uint32_t reserved;
	std::size_t executed;
};

extern "C" NativeEngineResult uml_x86_64_gadget_enter(
	const std::uintptr_t *program, CpuState *cpu,
	NativeExecutionContext *context);

extern "C" std::byte *uml_x86_64_gadget_tlb_miss(
	NativeExecutionContext *context, std::uint64_t address,
	std::uint32_t access, std::uint32_t size) noexcept;
extern "C" unsigned __int128 uml_x86_64_gadget_cross_page_load(
	NativeExecutionContext *context, std::uint64_t address,
	std::uint32_t size) noexcept;
extern "C" std::uint32_t uml_x86_64_gadget_cross_page_store(
	NativeExecutionContext *context, std::uint64_t address,
	std::uint64_t value, std::uint32_t size) noexcept;
extern "C" void uml_x86_64_gadget_publish_code_write(
	NativeExecutionContext *context, std::uint64_t address,
	std::uint32_t size) noexcept;
extern "C" void uml_x86_64_gadget_invalidate_code(
	NativeExecutionContext *context, std::uint64_t address) noexcept;
extern "C" void uml_x86_64_gadget_split_lock_enter(
	NativeExecutionContext *context) noexcept;
extern "C" void uml_x86_64_gadget_split_lock_leave(
	NativeExecutionContext *context) noexcept;
extern "C" std::uint32_t uml_x86_64_gadget_code_link_safe(
	NativeExecutionContext *context) noexcept;
extern "C" std::uint32_t uml_x86_64_gadget_cross_page_vector_load(
	NativeExecutionContext *context, std::uint64_t address,
	void *destination, std::uint32_t size) noexcept;
extern "C" std::uint32_t uml_x86_64_gadget_cross_page_vector_store(
	NativeExecutionContext *context, std::uint64_t address,
	const void *source, std::uint32_t size) noexcept;
extern "C" std::uint32_t uml_x86_64_gadget_bulk_load(
	NativeExecutionContext *context, std::uint64_t address,
	void *destination, std::uint32_t size) noexcept;
extern "C" std::uint32_t uml_x86_64_gadget_bulk_store(
	NativeExecutionContext *context, std::uint64_t address,
	const void *source, std::uint32_t size) noexcept;
/* RDRAND and RDSEED: host CSPRNG output, which carries nothing of the host's
 * own state into the guest. */
extern "C" std::uint64_t uml_x86_64_gadget_random_value() noexcept;
/* LSL of `selector`: sets ZF and writes the limit to the register whose
 * CpuState offset is `destination`'s low byte, at the width its next byte
 * names (1 a word, 2 a doubleword, 3 a quadword), or clears ZF. */
extern "C" void uml_x86_64_gadget_segment_limit(CpuState *cpu,
	std::uint32_t selector, std::uint32_t destination) noexcept;
/* Runs the instruction RIP names through the segment model, after the
 * gadget materialized the flags; returns a UML_X86_64_SEGMENT_* code. */
extern "C" std::uint32_t uml_x86_64_gadget_run_segment_instruction(
	NativeExecutionContext *context, CpuState *cpu) noexcept;

} // namespace uml::cpu::x86_64::gadgets
