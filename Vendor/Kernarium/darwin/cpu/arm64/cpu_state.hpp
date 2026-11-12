// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/sme_profile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace uml::cpu::arm64 {

/* The FPCR bits an MSR FPCR, a signal-frame or ptrace restore, or any other
 * context load can set on a host where the virtual CPU omits FEAT_AFP; the
 * rest read as zero and ignore writes. From the AArch64 FPCR description
 * (DDI 0601, 2026-06), for this virtual CPU:
 * - AHP, DN, FZ and RMode are unconditional fields;
 * - FZ16 exists with FEAT_FP16, which every supported host provides;
 * - FIZ, AH and NEP exist only with FEAT_AFP (fpcr_afp_controls), which the
 *   virtual CPU implements only where floating_controls.hpp says so;
 * - EBF exists only with FEAT_EBF16, which ID_AA64ISAR1_EL1.BF16 = 1 omits;
 * - the trap enables are RAZ/WI only without trapped exception handling,
 *   which both engines implement;
 * - Len and Stride serve only to save and restore the AArch32 FPSCR, and
 *   whether they are programmable or RAZ/WI is IMPLEMENTATION DEFINED.
 *   This CPU has no short vectors, so they are RAZ/WI.
 * This and fpcr_afp_controls are the one definition of the mask:
 * fpcr_writable_mask_for() selects the host's, the JIT, the context restore
 * path and the tests read it, and the gadget assembly's
 * UML_ARM64_FPCR_WRITABLE and UML_ARM64_FPCR_AFP_CONTROLS are checked
 * against them. */
inline constexpr std::uint32_t fpcr_writable_mask = 0x07c8'9f00U;

/* FPCR.NEP, AH and FIZ: writable where the virtual CPU implements FEAT_AFP,
 * RES0 everywhere else. */
inline constexpr std::uint32_t fpcr_afp_controls = 0x0000'0007U;
static_assert((fpcr_writable_mask & fpcr_afp_controls) == 0U);

/* The FPSR bits an MSR FPSR or any context load can set; the rest read as
 * zero and ignore writes. From the AArch64 FPSR description (DDI 0601,
 * 2026-06), for this virtual CPU: QC and the cumulative exception flags,
 * IDC included, are unconditional fields, and N, Z, C and V, the AArch32
 * floating-point comparison flags, exist only when AArch32 is implemented.
 * The hosts implement them as RAZ/WI, so the A64 FPSR omits them and an
 * AArch32 task keeps its flags in a32_fpscr_nzcv. The JIT, the context
 * restore path and the tests read this mask, and the gadget assembly's
 * UML_ARM64_FPSR_WRITABLE is checked against it. */
inline constexpr std::uint32_t fpsr_writable_mask = 0x0800'009fU;

/* The code-change sequence of a CpuState no engine has bound to an address
 * space. It never changes, so a context synchronization never leaves. */
inline constexpr std::uint64_t no_code_changes = 0;

/* The gadget engine's mark on a live exclusive_key, set only while
 * translated code runs: its executor entered translated code again since the
 * load-exclusive recorded the key, so its own code may have cleared the host
 * monitor (gadgets/engine/exclusive.inc). No host address has the bit set. */
inline constexpr unsigned exclusive_key_reentered_bit = 63;
inline constexpr std::uintptr_t exclusive_key_reentered =
	std::uintptr_t{1} << exclusive_key_reentered_bit;

struct alignas(16) VectorRegister final {
	std::array<std::byte, 16> bytes{};
};

struct CpuState final {
	std::array<std::uint64_t, 31> registers{};
	std::uint64_t sp{};
	std::uint64_t pc{};
	std::uint32_t nzcv{};
	std::array<VectorRegister, 32> vectors{};
	std::uint32_t fpcr{};
	std::uint32_t fpsr{};
	std::uint64_t tls{};
	/* Effective TBI0, imported from the task: EL0 data addresses ignore
	 * their top byte, whatever register is the base. */
	bool tbi_enabled{};
	/* EL0-visible PSTATE fields controlled by MSR (immediate).  DAIF access is
	 * explicitly gated because SCTLR_EL1.UMA determines whether userspace may
	 * use DAIFSet/DAIFClr; the other fields are effective feature state. */
	bool pstate_dit{};
	bool pstate_daif_access{};
	std::uint8_t pstate_daif{};
	std::uint64_t exclusive_address{~std::uint64_t{0}};
	std::uintptr_t exclusive_key{};
	std::uint64_t exclusive_value{};
	std::uint64_t exclusive_value2{};
	std::uint64_t exclusive_version{};
	std::uint8_t exclusive_size{};
	const std::byte *deferred_vectors{};
	std::uint32_t deferred_fpcr{};
	std::uint32_t deferred_fpsr{};
	bool fp_loaded{true};
	bool fp_dirty{};
	/* Read-only zero slot for translation-specialized production gadgets.
	 * Builder may map an architectural XZR source to this byte offset, avoiding
	 * a runtime register-number comparison. No destination is ever mapped here. */
	std::uint64_t gadget_zero{};
	/* Production may retain the guest FPCR/FPSR in the native registers across
	 * internal translated-block exits.  These private fields preserve the host
	 * environment until the outer guest-dispatch boundary. */
	std::uint64_t gadget_host_fpcr{};
	std::uint64_t gadget_host_fpsr{};
	bool gadget_fp_active{};
	/* Private execution-engine scratch, never guest-visible architectural
	 * state, holding one of two things. An indirect control-flow cache miss
	 * records its data-only cache cell here, with a `continued` event, so the
	 * gadget executor can train that exact site. Every other exit that
	 * writes it records its position in a translated stream instead, and
	 * does so only with an event that does not retire the instruction or
	 * from the FPCR write that enables traps; the executor consumes and
	 * clears that pointer before it trains, so a stream pointer is never
	 * taken for a cache cell. A new exit that records its stream must keep
	 * to that rule. */
	void *gadget_indirect_miss{};
	/* Private gadget-engine return predictor: linked calls push the return
	 * PC with the BlockLink of its block, and a cached return whose site
	 * cells miss pops the newest record when it names the live link
	 * register. Records name link state of one address space, so the
	 * executor empties the ring on every address-space switch and after a
	 * link sweep releases records. A stale or mismatched record can only
	 * fall back to the ordinary miss path. */
	struct ReturnRecord final {
		/* An unaligned sentinel: no return PC can ever match an empty
		 * record, so an empty ring never follows its null link. */
		std::uint64_t pc{~std::uint64_t{0}};
		const void *link{};
	};
	/* Shared with assembly_offsets.h; a power of two no larger than 256. */
	static constexpr std::size_t return_stack_entries = 16;
	std::array<ReturnRecord, return_stack_entries> gadget_return_stack{};
	std::uint8_t gadget_return_top{};
	/* Private gadget-engine target table of the running address space: a
	 * direct-mapped array of {guest PC, BlockLink} records, indexed by a
	 * fold of the target address, that a cached indirect branch consults
	 * once its site cells -- and for a return the ring -- have missed. The
	 * executor publishes it before every translated run; null turns the
	 * lookup off. Like the ring, a stale or mismatched record can only fall
	 * back to the ordinary miss path. */
	const void *gadget_target_tier{};
	/* SME state, appended so that no earlier offset moves. TPIDR2_EL0, then
	 * SVCR (svcr_sm, svcr_za). sme_loaded marks the parts of the register
	 * file access_sme_state() filled from the task in this entry, and
	 * sme_dirty those write_back_sme_state() must store, with SVCR's bits:
	 * Z/P for SM, ZA/ZT0 for ZA. sme_host_mode is engine-private host
	 * state (sme_host_*). */
	std::uint64_t tpidr2{};
	std::uint8_t svcr{};
	std::uint8_t sme_loaded{};
	std::uint8_t sme_dirty{};
	std::uint8_t sme_host_mode{};
	/* The streaming vector length in bytes the register file is sized for,
	 * 0 without one. */
	std::uint32_t sme_svl{};
	/* The task's SME storage, in the register file's layout, or null when
	 * the task has none; the kernel owns it. */
	std::byte *sme_task{};
	/* This vCPU's register file (SmeRegisterFile), sme_storage_size(sme_svl)
	 * bytes, or null when the run does not implement SME. While SM is set,
	 * guest V_n is the low 16 bytes of Z_n here and `vectors` is stale. */
	std::byte *sme_registers{};
	/* Bytes 1232 to 1507 are the storage of the removed non-streaming SVE
	 * profile, kept so that no offset after them moves. The virtual CPU has
	 * no SVE outside streaming mode, so nothing reads or writes them. */
	alignas(16) std::array<std::byte, 276> reserved_sve{};
	/* AArch32 EL0 state (cpu/arm64/aarch32/a32_profile.hpp), appended so that
	 * no earlier offset moves. An AArch32 task keeps R0-R14 in registers[0..14]
	 * zero-extended, N, Z, C and V in nzcv, D0-D31 in the low sixteen vectors,
	 * FPSCR's control bits in fpcr and its status bits in fpsr, TPIDRURW in
	 * tls, and its PC in key form (aarch32/guest_key.hpp) for the whole entry;
	 * these are the parts that have no A64 field. The runtime writes every one
	 * of them on each AArch32 entry and an A64 entry never reads them.
	 * registers[15..30] and sp hold what the virtual CPU ran before, an
	 * AArch64 task's x15-x30 and SP among it, and an engine that borrows one
	 * sets it before it reads it.
	 *
	 * a32 marks an AArch32 entry for assertions and the sampler. a32_q is the
	 * sticky Q flag. a32_ge_mask is GE[3:0] with each flag widened to the byte
	 * lane it governs (0xff when set), so SEL is one bitwise select.
	 * a32_itstate is ITSTATE; it is zero at every cached block boundary, and
	 * an entry in the middle of an IT block runs one instruction at a time.
	 * a32_fpscr_nzcv holds FPSCR.N, Z, C and V in bits 31:28: the hosts
	 * implement FPSR[31:28] as RAZ/WI, so a VCMP's flags live here.
	 * a32_tpidruro is TPIDRURO, which EL0 can read but not write. a32_cp15ben
	 * is 1 when the CP15 barrier instructions are enabled (SCTLR.CP15BEN) and
	 * 0 when they are UNDEFINED. */
	std::uint32_t a32_ge_mask{};
	std::uint32_t a32_fpscr_nzcv{};
	std::uint32_t a32_tpidruro{};
	std::uint8_t a32_q{};
	std::uint8_t a32_itstate{};
	std::uint8_t a32_cp15ben{};
	std::uint8_t a32{};
	/* The AArch32 engines' native floating-point environment, while
	 * gadget_fp_active is set, holds the Standard FPSCR value that Advanced
	 * SIMD floating point runs under when a32_fp_standard is 1 and the
	 * guest's FPCR when it is 0. The gadgets (engine/a32.inc) and the JIT's
	 * translations that need one switch to it, and what activates the
	 * environment clears this; it means nothing while gadget_fp_active is
	 * clear. */
	std::uint8_t a32_fp_standard{};
	/* Context synchronization, appended so that no earlier offset moves:
	 * the running address space's code-change sequence
	 * (darwin_uml_static_vm_execution_context_code_sequence) and the value
	 * of it that the translations this entry runs were validated at. Both
	 * engines publish them before every translated run. An ISB compares the
	 * two and leaves translated code when they differ, so code another vCPU
	 * changed and invalidated before this one synchronized is fetched anew
	 * rather than run from a stale translation or link. */
	const std::uint64_t *code_sequence{&no_code_changes};
	std::uint64_t code_sequence_seen{};
	/* The JIT's staging for an Advanced SIMD load that crosses between two
	 * pages whose host mappings are not adjacent: the last 32 bytes of the
	 * first page, then the first 32 of the second. It holds nothing between
	 * two instructions. */
	alignas(16) std::array<std::uint8_t, 64> a32_cross{};
	[[nodiscard]] std::uint64_t untag_data_address(
		std::uint64_t address) const noexcept
	{
		return tbi_enabled ?
			address & 0x00ff'ffff'ffff'ffffULL : address;
	}

	/* Drops the exclusive monitor: no address, key, value or size is held. */
	void clear_exclusive_monitor() noexcept
	{
		exclusive_address = ~std::uint64_t{0};
		exclusive_key = 0;
		exclusive_value = 0;
		exclusive_value2 = 0;
		exclusive_version = 0;
		exclusive_size = 0;
	}

	/* Loads a saved context lazily. Its FPCR is masked as an MSR FPCR
	 * write is, with the host's fpcr_writable_mask_for(), and its FPSR as
	 * an MSR FPSR write is. */
	void defer_fp_load(const void *source, std::uint32_t source_fpcr,
			  std::uint32_t source_fpsr,
			  std::uint32_t fpcr_writable) noexcept
	{
		deferred_vectors = static_cast<const std::byte *>(source);
		deferred_fpcr = source_fpcr & fpcr_writable;
		deferred_fpsr = source_fpsr & fpsr_writable_mask;
		fp_loaded = false;
		fp_dirty = false;
	}

	void access_fp_state() noexcept
	{
		if (!fp_loaded) {
			std::memcpy(vectors.data(), deferred_vectors, sizeof(vectors));
			fpcr = deferred_fpcr;
			fpsr = deferred_fpsr;
			fp_loaded = true;
		}
		fp_dirty = true;
	}

	/* Points this vCPU at its register file; see SmeRegisterFile. */
	void bind_sme_registers(const SmeRegisterFile &file) noexcept
	{
		sme_registers = file.data();
		sme_svl = file.svl();
	}

	/* Loads a task's SME state lazily, as defer_fp_load() loads its FP
	 * state: SVCR and TPIDR2_EL0 now, Z/P and ZA/ZT0 only when
	 * access_sme_state() needs them. */
	void defer_sme_load(std::byte *task, std::uint8_t task_svcr,
			    std::uint64_t task_tpidr2) noexcept
	{
		sme_task = task;
		svcr = task_svcr & svcr_mask;
		tpidr2 = task_tpidr2;
		sme_loaded = 0;
		sme_dirty = 0;
	}

	/* Makes the state SVCR makes live accessible in the register file and
	 * marks it dirty, as access_fp_state() does for V: Z/P while SM is
	 * set, with each Z's low 16 bytes taken from the FP image's V (so the
	 * FP state is accessed first), and ZA/ZT0 while ZA is set. A task with
	 * no storage contributes zeros, so no earlier task's state is ever
	 * exposed. An engine calls it before it first runs code whose mode has
	 * SM or ZA set. */
	void access_sme_state() noexcept
	{
		if (sme_registers == nullptr)
			return;
		const auto copy_from_task = [this](std::byte *destination,
			std::size_t offset, std::size_t size) noexcept {
			if (sme_task != nullptr)
				std::memcpy(destination, sme_task + offset, size);
			else
				std::memset(destination, 0, size);
		};
		const auto load = static_cast<std::uint8_t>(svcr & ~sme_loaded);
		if ((load & svcr_sm) != 0U) {
			access_fp_state();
			for (unsigned n = 0; n < sme_z_registers; ++n) {
				auto *z = sme_registers + sme_z_offset(sme_svl, n);
				std::memcpy(z, vectors[n].bytes.data(), 16);
				copy_from_task(z + 16, sme_z_offset(sme_svl, n) + 16,
					sme_svl - 16U);
			}
			copy_from_task(sme_registers + sme_p_offset(sme_svl, 0),
				sme_p_offset(sme_svl, 0),
				sme_p_registers * (sme_svl / 8U));
		}
		if ((load & svcr_za) != 0U)
			copy_from_task(sme_registers + sme_za_offset(sme_svl, 0),
				sme_za_offset(sme_svl, 0),
				sme_storage_size(sme_svl) -
					sme_za_offset(sme_svl, 0));
		sme_loaded |= svcr;
		sme_dirty |= svcr;
	}

	/* Stores what access_sme_state() exposed, or a write of SVCR zeroed,
	 * back into the task, as the runtime does for V before it exports the
	 * task frame: Z/P while SM is set, copying each Z's low 16 bytes into
	 * the FP image's V as well, and ZA/ZT0 while ZA is set. */
	void write_back_sme_state() noexcept
	{
		if (sme_registers == nullptr || sme_task == nullptr)
			return;
		const auto store = static_cast<std::uint8_t>(sme_dirty & svcr);
		if ((store & svcr_sm) != 0U) {
			std::memcpy(sme_task, sme_registers,
				sme_za_offset(sme_svl, 0));
			access_fp_state();
			for (unsigned n = 0; n < sme_z_registers; ++n)
				std::memcpy(vectors[n].bytes.data(),
					sme_registers + sme_z_offset(sme_svl, n),
					16);
		}
		if ((store & svcr_za) != 0U)
			std::memcpy(sme_task + sme_za_offset(sme_svl, 0),
				sme_registers + sme_za_offset(sme_svl, 0),
				sme_storage_size(sme_svl) -
					sme_za_offset(sme_svl, 0));
	}

	/* Executes a guest write of SVCR by svcr_write()'s rules: the bits in
	 * `mask` take their values from `value`. No native FP environment may
	 * hold a newer guest FPSR than CpuState, and the register file must
	 * exist. Returns false, changing nothing, when the task has no SME
	 * storage and the write would set SM or ZA: the engine then exits with
	 * sme_access without retiring the instruction. */
	[[nodiscard]] bool write_svcr(std::uint8_t mask, std::uint8_t value) noexcept
	{
		const auto write = svcr_write(svcr, mask, value, sme_task != nullptr);
		if (write.needs_storage)
			return false;
		if (write.reset_fpsr) {
			access_fp_state();
			fpsr = streaming_mode_change_fpsr;
		}
		if (write.zero_streaming)
			std::memset(sme_registers, 0, sme_za_offset(sme_svl, 0));
		if (write.zero_vectors)
			vectors = {};
		if (write.zero_za)
			std::memset(sme_registers + sme_za_offset(sme_svl, 0), 0,
				sme_storage_size(sme_svl) -
					sme_za_offset(sme_svl, 0));
		const auto zeroed = static_cast<std::uint8_t>(
			(write.zero_streaming ? svcr_sm : 0U) |
			(write.zero_za ? svcr_za : 0U));
		sme_loaded |= zeroed;
		sme_dirty |= zeroed;
		svcr = write.svcr;
		return true;
	}

	/* The guest FPCR, whether or not the deferred image is loaded yet. */
	[[nodiscard]] std::uint32_t effective_fpcr() const noexcept
	{
		return fp_loaded ? fpcr : deferred_fpcr;
	}

	/* The guest FPSR, whether or not the deferred image is loaded yet. */
	[[nodiscard]] std::uint32_t effective_fpsr() const noexcept
	{
		return fp_loaded ? fpsr : deferred_fpsr;
	}

	[[nodiscard]] std::uint64_t read_register(unsigned index,
					     bool stack_pointer) const noexcept
	{
		if (index < registers.size())
			return registers[index];
		return stack_pointer ? sp : 0;
	}

	void write_register(unsigned index, std::uint64_t value,
			    bool stack_pointer) noexcept
	{
		if (index < registers.size())
			registers[index] = value;
		else if (stack_pointer)
			sp = value;
	}
};

} // namespace uml::cpu::arm64
