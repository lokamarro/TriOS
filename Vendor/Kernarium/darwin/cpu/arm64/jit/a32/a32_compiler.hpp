// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/jit/jit_compiler.hpp"
#include "cpu/arm64/jit/jit_compiler_families.hpp"
#include "a32_operations.hpp"
#include "a32_transfer.hpp"
#include "a32_vector_operations.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <vector>

namespace uml::cpu::arm64::jit::a32 {

/* What the compiler is asked to translate: the guest bytes at the key's
 * fetch address (little endian, as the guest memory holds them), as many as
 * the executor could read. */
struct Request final {
  std::uint64_t key{};
  std::span<const std::byte> bytes;
  /* ITSTATE the entry holds. Nonzero only for a single-instruction
   * translation, which every other translation starts outside an IT block
   * (aarch32/guest_key.hpp). */
  aarch32::ItState itstate{};
  /* Translate exactly the first instruction, which stores the ITSTATE that
   * follows it instead of leaving it to the next block. */
  bool single{};
};

struct Result final {
  JitCompiler::Result translation;
  /* The guest units the translation consumed: words for A32, halfwords for
   * T32 (aarch32::isa_unit_bytes of the key's ISA). The translation's
   * guest_instructions counts instructions. */
  std::size_t guest_units{};
};

/* The AArch32 compiler of the JIT: a block of A32 or T32
 * instructions decoded by the shared decoder and lowered onto the same
 * residency, flag state and linked exits as the A64 compiler, with the guest
 * PC in key form. R0-R14 are CpuState registers[0..14], zero-extended; all
 * integer arithmetic is on W registers, so every result keeps that form. */
class A32Compiler final : private JitCompilerFamilies {
public:
  using JitCompilerFamilies::IndirectLinkResolver;
  using JitCompilerFamilies::LinkResolver;

  explicit A32Compiler(void *link_context = nullptr,
                       LinkResolver link_resolver = nullptr,
                       bool track_direct_stores = false,
                       IndirectLinkResolver indirect_link_resolver =
                           nullptr) noexcept;

  [[nodiscard]] Result compile(const Request &request);

private:
  using Instruction = aarch32::Instruction;
  using Form = aarch32::Form;
  using Isa = aarch32::Isa;

  struct Decoded final {
    Instruction inst;
    /* The instruction's AArch32 address. */
    std::uint32_t pc{};
    /* ITSTATE before it. */
    std::uint8_t it{};
  };

  enum class Flow : std::uint8_t {
    next,
    /* The instruction ended the block: a branch, an exit, a helper call. */
    end,
    /* The block ends before the instruction, which is not part of it. */
    split,
  };

  struct ColdPath final {
    enum class Kind : std::uint8_t {
      /* The access faulted: report it at the instruction. */
      memory_fault,
      /* The instruction the hot path declined (a memory access that is
       * unaligned, crosses a page or needs the helper, or a form with no
       * inline lowering): the C++ helper runs it. */
      slow,
      /* The exclusive the hot path declined: the C++ helper runs it. */
      exclusive_slow,
      /* Retry exit of a fused exclusive region. */
      exclusive_mismatch,
      /* The instruction is UNDEFINED at run time (a CP15 barrier while
       * SCTLR.CP15BEN is clear). */
      illegal,
      /* A conditional branch the block went on past was taken: link to
       * `target`. */
      branch,
      /* An ISB found the address space's code changed: leave after it. */
      code_changed,
    };
    Arm64Emitter::Label label;
    Kind kind;
    std::size_t index;
    std::uint16_t dirty_gprs{};
    std::uint32_t dirty_vectors{};
    GuestFlagsState flags{GuestFlagsState::memory};
    std::uint64_t target{};
    /* A branch taken inside a fused exclusive run: the host's monitor is
     * open and the exit closes it. */
    bool monitor{};
  };

  /* A guarded floating-point operation whose operands or result were not
   * clean: it runs under the Standard FPSCR value and returns. */
  struct FloatColdPath final {
    Arm64Emitter::Label label;
    Arm64Emitter::Label continuation;
    std::uint32_t base;
    bool quad;
    unsigned dest;
    unsigned a;
    unsigned b;
  };

  /* The skipped arm of a fused conditional store: it clears the monitor and
   * resumes after the store. */
  struct ResumePath final {
    Arm64Emitter::Label label;
    Arm64Emitter::Label continuation;
  };

  struct TranslationColdPath final {
    Arm64Emitter::Label label;
    Arm64Emitter::Label continuation;
    Arm64Emitter::Label fault;
    unsigned address;
    ::uml::memory::Access access;
    /* Nonzero when the hot path's tag compare was against the last byte of
     * this many: the cold path first sends a crossing access to `crossing`. */
    std::size_t bytes{};
    Arm64Emitter::Label crossing{};
    /* The access takes the delta (x9) and the guest address (x8) and adds
     * them itself: the cold path leaves the host pointer in x9 and zero in
     * x8. */
    bool fold{};
  };

  /* An Advanced SIMD load whose span crosses a page: the cold path probes
   * both pages and either reads through the first page's mapping (the two are
   * adjacent in the host) or stages the bytes and reads those. A miss on
   * either page runs the instruction in the library. */
  struct CrossingColdPath final {
    Arm64Emitter::Label label;
    Arm64Emitter::Label continuation;
    Arm64Emitter::Label slow;
    unsigned address;
    std::size_t bytes;
    /* As in TranslationColdPath: the continuation takes x9 + w8. */
    bool fold{};
  };

  /* A host register holding a W value and, for a destination, how it is
   * committed. */
  struct Dest final {
    NativeGpr native;
    unsigned guest;
  };

  /* A block whose last instruction branches back to its own first loops in
   * the host: the registers it holds in the host through the block are
   * loaded once before the loop head, the back edge costs a budget check
   * and a branch, and an exit writes back what the iteration left. The
   * plan is the bindings a probe compilation held at the back edge. */
  struct LoopPlan final {
    bool valid{};
    /* The body guards Neon floating-point operations and has nothing that
     * needs the guest's FPCR: the loop runs under the Standard value, set
     * once before it. */
    bool standard_fp{};
    std::array<unsigned, 15> gpr{};
    std::array<unsigned, 18> vector{};
  };

  /* The register bindings a conditional body must leave as it found them. */
  struct ResidencyMark final {
    decltype(JitCompilerFamilies::gpr_slots_) gpr;
    decltype(JitCompilerFamilies::vector_slots_) vector;
  };
  /* How a logical operation sets the carry flag. */
  enum class CarryKind : std::uint8_t {
    /* C keeps its value. */
    unchanged,
    clear,
    set,
    /* C is the bit of carry_reg selected by a rotation (RMIF). */
    bit,
    /* C is bit 1 of x0, from a register-controlled shift. */
    computed,
  };

  // a32_compiler.cpp: the driver.
  void decode_block(const Request &request);
  void emit_block();
  Flow emit_instruction();
  Flow emit_guarded(unsigned condition);
  /* Whether the block goes on past the instruction, which leaves by a side
   * exit when it is taken: a conditional branch forward (continues_past), or
   * back to anywhere but the block's own first instruction, which the host
   * loop of a block that branches back to itself covers. */
  [[nodiscard]] bool continues_through(const Decoded &d) const;
  /* Whether the conditional instruction can run on both arms and keep its
   * result only where the condition holds: a data-processing instruction
   * (or MOVW) that leaves the host flags alone (it sets none and shifts by
   * no register) and does not write the PC. */
  [[nodiscard]] bool can_predicate() const;
  [[nodiscard]] Flow emit_side_exit_branch(bool monitor = false);
  [[nodiscard]] bool loop_candidate(const Request &request);
  void record_loop_plan();
  void apply_loop_plan();
  /* The loads of the core registers that precede the internal entry, and the
   * bindings that say they are there. */
  void load_kept_gprs();
  [[nodiscard]] Flow emit_loop_branch();
  Flow emit_predicated(unsigned condition);
  [[nodiscard]] Flow emit_operation();
  void emit_cold_paths();
  void emit_slow_helper(const ColdPath &cold);
  void emit_deferred_exclusive(const ColdPath &cold);
  [[nodiscard]] ResidencyMark mark_residency() const;
  void restore_residency(const ResidencyMark &mark);
  void close_conditional_body(const ResidencyMark &mark);

  // Exits.
  [[nodiscard]] std::uint64_t key_at(std::uint32_t pc) const noexcept;
  [[nodiscard]] std::uint64_t key_of(const Decoded &d) const noexcept {
    return key_at(d.pc);
  }
  [[nodiscard]] std::uint64_t next_key() const noexcept {
    return key_at(cur().pc + cur().inst.length);
  }
  void store_it(std::uint8_t value);
  void exit_event(Event event, std::uint64_t pc_key, std::uint8_t it);
  void exit_illegal();
  /* A static successor: linked when the ITSTATE that follows is zero. The
   * one-argument form is the exit of the instruction being translated. */
  void exit_direct(std::uint64_t target_key);
  void exit_direct(std::uint64_t target_key, std::uint8_t it,
                   std::size_t count);
  void exit_indirect(unsigned key_register);
  void exit_continue_at(std::uint64_t pc_key, std::uint8_t it,
                        std::size_t count);
  [[nodiscard]] Arm64Emitter::Label cold_exit(ColdPath::Kind kind);
  /* After an ISB: leave after it when the address space's code changed
   * since this run's translations were validated, as the A64 compiler's
   * check does (JitCompiler::emit_code_change_check). */
  void emit_code_change_check();

  // Registers and constants (a32_compiler_alu.cpp).
  [[nodiscard]] const Decoded &cur() const noexcept { return block_[count_]; }
  [[nodiscard]] std::uint32_t pc_read() const noexcept;
  [[nodiscard]] std::uint32_t pc_align_read() const noexcept {
    return pc_read() & ~3U;
  }
  /* The host registers the lowering borrows. A guest register that is not in
   * a resident host register (the PC, which reads as a constant) is staged
   * in fallback_n or fallback_m; address_register holds a transfer's address;
   * temp0 to temp2 are integer temporaries; the s_ and scratch_ numbers name
   * the vector registers that carry the operands, accumulator and result of
   * a Neon or VFP form. */
  static constexpr unsigned fallback_n = 9;
  static constexpr unsigned fallback_m = 10;
  static constexpr unsigned address_register = 8;
  static constexpr unsigned temp0 = 0;
  static constexpr unsigned temp1 = 1;
  static constexpr unsigned temp2 = 8;
  static constexpr unsigned s_n = 0;
  static constexpr unsigned s_m = 1;
  static constexpr unsigned s_acc = 2;
  static constexpr unsigned s_result = 5;
  static constexpr unsigned s_extra = 6;
  static constexpr unsigned scratch_n = 3;
  static constexpr unsigned scratch_m = 4;
  static constexpr unsigned scratch_result = 5;
  /* The host register holding guest register r (the PC's value for r15). */
  [[nodiscard]] unsigned use(unsigned r, unsigned fallback);
  [[nodiscard]] Dest def(unsigned r, bool reads_old, unsigned fallback);
  void done(const Dest &dest);
  void load_u32(unsigned destination, std::uint32_t value);
  void clobber_flags();

  // Data processing (a32_compiler_alu.cpp).
  [[nodiscard]] Flow emit_data_processing(const OpInfo &info);
  [[nodiscard]] Flow emit_address_forms(const OpInfo &info);
  [[nodiscard]] unsigned shift_value(unsigned source, aarch32::SRType type,
                                     unsigned amount, unsigned scratch);
  void shift_by_register(unsigned out, unsigned rm, unsigned rs,
                         aarch32::SRType type);
  void shift_by_register_carry(unsigned out, unsigned rm, unsigned rs,
                               aarch32::SRType type);
  void restore_logical_flags(CarryKind carry, unsigned carry_reg,
                             unsigned carry_rot);

  // Memory (a32_compiler_memory.cpp).
  [[nodiscard]] Flow emit_memory(const OpInfo &info);
  [[nodiscard]] Flow emit_single(const Transfer &transfer);
  [[nodiscard]] Flow emit_pair(const Transfer &transfer);
  [[nodiscard]] Flow emit_multiple(const Transfer &transfer);
  [[nodiscard]] Flow emit_table(const Transfer &transfer);
  [[nodiscard]] Flow emit_exclusive(const Transfer &transfer);
  /* The whole instruction in the slow helper. */
  [[nodiscard]] Flow emit_through_helper();
  /* The load of `bytes` (at most CrossingColdPath's limit) at `address`
   * branches to `crossing` instead of `slow` when it spans two pages.
   * `aligned` is the alignment the caller has already checked: an access no
   * larger than that cannot span two pages, and is not tested for it. */
  void emit_translate(unsigned address, ::uml::memory::Access access,
                      Arm64Emitter::Label slow, Arm64Emitter::Label fault,
                      std::size_t bytes,
                      std::optional<Arm64Emitter::Label> crossing =
                          std::nullopt,
                      unsigned aligned = 0, bool fold = false);
  /* x1 takes the page number of `address` and x0 the TLB entry's address. */
  void emit_tlb_index(unsigned address);
  void emit_translation_cold(const TranslationColdPath &path);
  void emit_crossing_cold(const CrossingColdPath &path);
  void emit_alignment_check(unsigned address, unsigned alignment,
                            Arm64Emitter::Label slow);
  /* The address of a single or paired transfer in x8 (the base for a
   * post-indexed one). */
  void form_address(const Transfer &transfer);
  /* destination = base plus or minus the shifted register offset m. */
  void emit_register_offset(unsigned destination, unsigned base,
                            const Transfer &transfer, unsigned m);
  /* The base register after a writeback transfer, from the registers as
   * they stand (they hold the values the address used). */
  void write_back(const Transfer &transfer);
  void mark_memory_write() noexcept;
  void mark_direct_memory_write() noexcept;
  void track_direct_store(unsigned address, std::size_t size);
  /* LL/SC fusion (a32_compiler_exclusive.cpp): the number of instructions of
   * the block's start it lowered to a host load-exclusive/store-exclusive
   * run, or 0 when it declined. */
  [[nodiscard]] std::size_t try_fuse_exclusive();
  /* Ends the software monitor: a CLREX and the end of a fused pair. */
  void clear_exclusive_monitor();

  // Multiplies, bit manipulation, extension, saturation and DSP forms
  // (a32_compiler_arith.cpp).
  [[nodiscard]] Flow emit_multiply(const OpInfo &info);
  [[nodiscard]] Flow emit_bit_operation(const OpInfo &info);
  [[nodiscard]] Flow emit_extend(const OpInfo &info);
  [[nodiscard]] Flow emit_saturate(const OpInfo &info);
  [[nodiscard]] Flow emit_dsp_multiply(const OpInfo &info);
  [[nodiscard]] Flow emit_parallel(const OpInfo &info);
  /* The whole instruction in the slow helper. */
  [[nodiscard]] Flow emit_arith_through_helper();
  void emit_extract(unsigned out, unsigned source, bool is_signed,
                    unsigned bits, unsigned rotation);
  void emit_half(unsigned out, unsigned source, unsigned index);
  void or_q(unsigned flag);
  void saturating_add(unsigned out, unsigned a, unsigned b, bool subtract,
                      bool accumulate);
  void set_q_on_overflow();

  // Vectors: the register views and the floating-point environment
  // (a32_compiler_vector.cpp).
  [[nodiscard]] Flow emit_vector(const OpInfo &info);
  /* Q n is CpuState vector n: its host register holds the whole register. */
  [[nodiscard]] unsigned q_use(unsigned q, unsigned fallback);
  [[nodiscard]] NativeVector q_def(unsigned q, bool reads_old,
                                   unsigned fallback);
  void q_done(NativeVector value, unsigned q);
  /* D n is the low (even n) or high (odd n) half of Q n/2. The returned
   * host register holds the doubleword in its low 64 bits (the rest is
   * unspecified), staged in `scratch` when it is not already there. */
  [[nodiscard]] unsigned d_use(unsigned d, unsigned scratch);
  /* The doubleword in both halves of the returned register. */
  [[nodiscard]] unsigned d_use_twice(unsigned d, unsigned scratch);
  /* D n takes the low 64 bits of `result`; the other half is kept. */
  void d_write(unsigned d, unsigned result);
  /* S n is lane n%4 of Q n/4: the returned register holds it in its low 32
   * bits. */
  [[nodiscard]] unsigned s_use(unsigned s, unsigned scratch);
  void s_write(unsigned s, unsigned result);
  /* S shadows: in a run of instructions that leave V0-V4 and V7 alone, an
   * S register that was read from its lane or written keeps a copy in the
   * low lane of one of them, which the next read takes instead of the lane.
   * The Q register still takes every write, so an exit, a link or a cold
   * path never needs a shadow. begin_s_shadows() classifies the instruction
   * about to be translated: it keeps, forgets or may use the shadows. */
  void begin_s_shadows();
  void forget_s_shadows() noexcept;
  void forget_s_shadow(unsigned s) noexcept;
  /* The host register of S n's shadow, which the instruction reads, or -1
   * where it has none or may not use them. */
  [[nodiscard]] int s_shadow_host(unsigned s) noexcept;
  /* The host register of the shadow S n takes, an existing one or the least
   * recently used one the instruction does not read. */
  [[nodiscard]] unsigned take_s_shadow(unsigned s) noexcept;
  /* The register the last instruction of a VFP operation on S or D n
   * writes, for fp_write: S n's shadow when the instruction may use them. */
  [[nodiscard]] unsigned fp_result(unsigned r, bool dp) noexcept;
  /* A VFP operand: a single or a double register. */
  [[nodiscard]] unsigned fp_use(unsigned r, bool dp, unsigned scratch) {
    return dp ? d_use(r, scratch) : s_use(r, scratch);
  }
  void fp_write(unsigned r, bool dp, unsigned result) {
    if (dp)
      d_write(r, result);
    else
      s_write(r, result);
  }
  /* The instruction reads or writes vector or floating-point state. */
  void touch_vectors() noexcept { accesses_fp_state_ = true; }
  /* The instruction depends on the host FPCR or FPSR, or sets FPSR. */
  void touch_fp_environment();
  /* Moves the host FPCR to the guest's value or to the Standard FPSCR
   * value (FZ and DN set, round to nearest, AHP and FZ16 the guest's), when
   * it is not there already: the translation knows the mode only after an
   * instruction of its own, and tests CpuState::a32_fp_standard for the
   * first. */
  void set_fp_mode(bool standard);
  [[nodiscard]] std::uint32_t neon_word() const noexcept;

  // Vector memory (a32_compiler_vmem.cpp).
  [[nodiscard]] Flow emit_vector_memory(const VInfo &v);
  [[nodiscard]] Flow emit_vector_register_transfer(bool store);
  [[nodiscard]] Flow emit_vector_block_transfer(bool store);
  [[nodiscard]] Flow emit_vector_structure(const VInfo &v);
  [[nodiscard]] Flow vmem_through_helper(bool store);
  void vmem_granule(bool load, unsigned log2_size, unsigned index,
                    unsigned offset);
  [[nodiscard]] bool vmem_folds(bool store, unsigned bytes, unsigned first,
                                unsigned per_q) const;
  void vmem_run(bool load, bool single, unsigned first, unsigned count,
                bool fold = false);
  void vmem_d_from(unsigned dreg, unsigned source, unsigned half);
  void vmem_d_into(unsigned dreg, unsigned scratch, unsigned half);

  // The Armv8 extensions (a32_compiler_armv8.cpp).
  [[nodiscard]] Flow emit_extension(const OpInfo &info);
  [[nodiscard]] Flow emit_crc32();
  [[nodiscard]] Flow emit_crypto();
  [[nodiscard]] Flow emit_vsel();
  [[nodiscard]] Flow emit_vmaxnm();
  [[nodiscard]] Flow emit_vrint();
  [[nodiscard]] Flow emit_vcvt_round();
  [[nodiscard]] Flow emit_vjcvt();
  [[nodiscard]] Flow emit_dot(bool by_element);
  [[nodiscard]] Flow emit_rdm();
  [[nodiscard]] Flow emit_vcadd();
  [[nodiscard]] Flow emit_vcmla(bool by_element);

  // VFP (a32_compiler_vfp.cpp).
  [[nodiscard]] Flow emit_vfp(const OpInfo &info, const VInfo &v);
  [[nodiscard]] Flow emit_vfp_arith(const VInfo &v);
  [[nodiscard]] Flow emit_vfp_compare(const VInfo &v);
  /* Whether the instruction at `index` is an unconditional VMRS that moves
   * the FPSCR flags to the APSR. */
  [[nodiscard]] bool is_fpscr_flags_read(std::size_t index) const;
  [[nodiscard]] Flow emit_vfp_convert(const VInfo &v);
  [[nodiscard]] Flow emit_vfp_move(const VInfo &v);
  [[nodiscard]] Flow emit_fpscr(const VInfo &v);

  // Advanced SIMD (a32_compiler_neon*.cpp).
  [[nodiscard]] Flow emit_neon(const VInfo &v);
  [[nodiscard]] bool neon_half_precision(const VInfo &v) const;
  [[nodiscard]] Flow emit_neon_same(const VInfo &v);
  /* VMAX and VMIN (single precision) under the guest's FPCR, where the
   * translation holds it: see emit_neon_minmax_guarded. */
  [[nodiscard]] Flow emit_neon_minmax_guarded(const VInfo &v);
  /* VADD, VSUB, VMUL, VMLA and VMLS (single precision) the same way: see
   * emit_guarded_float_op. */
  [[nodiscard]] Flow emit_neon_float_guarded(const VInfo &v);
  [[nodiscard]] bool neon_float_isolated() const;
  void emit_float_special_test(unsigned a, unsigned b, bool quad,
                               unsigned live);
  void emit_float_tiny_test(unsigned result, bool quad, unsigned live);
  void emit_float_product_test(unsigned a, unsigned b, bool quad,
                               unsigned live);
  void emit_guarded_float_op(std::uint32_t base, bool quad, unsigned dest,
                             unsigned a, unsigned b, bool product,
                             unsigned live);
  void emit_fp_write(bool standard);
  [[nodiscard]] Flow emit_neon_step(bool rsqrt, bool quad, unsigned d,
                                    unsigned n, unsigned m);
  [[nodiscard]] Flow emit_neon_logic(const VInfo &v);
  [[nodiscard]] Flow emit_neon_misc(const VInfo &v);
  [[nodiscard]] Flow emit_neon_convert(const VInfo &v);
  [[nodiscard]] Flow emit_neon_immediate(const VInfo &v);
  [[nodiscard]] Flow emit_neon_permute(const VInfo &v);
  [[nodiscard]] Flow emit_neon_table(const VInfo &v);
  [[nodiscard]] Flow emit_neon_shift(const VInfo &v);
  [[nodiscard]] Flow emit_neon_diff(const VInfo &v);
  [[nodiscard]] Flow emit_neon_scalar(const VInfo &v);
  [[nodiscard]] static unsigned esize_log2(int esize) noexcept;
  /* An A64 vector instruction with its register fields clear, given them. */
  void emit_vec3(std::uint32_t base, unsigned rd, unsigned rn, unsigned rm) {
    emit(base | rd | (rn << 5) | (rm << 16));
  }
  void emit_vec2(std::uint32_t base, unsigned rd, unsigned rn) {
    emit(base | rd | (rn << 5));
  }
  /* Register copy (a no-op when they are the same register). */
  void vmov(unsigned d, unsigned n);
  /* The register or half register `reg` (a D or Q number) as a source. */
  [[nodiscard]] unsigned vsrc(unsigned reg, bool quad, unsigned scratch);
  void neon_op3(std::uint32_t base, bool quad, bool wide, unsigned d,
                unsigned n, unsigned m, bool reads_dest);
  void neon_op2(std::uint32_t base, bool quad, bool wide, unsigned d,
                unsigned m, bool reads_dest);
  void q_put(unsigned q, unsigned result);
  void neon_put(unsigned reg, bool quad, unsigned result);

  // Control (a32_compiler_control.cpp).
  [[nodiscard]] Flow emit_branch(const OpInfo &info);
  [[nodiscard]] Flow emit_status_register(const OpInfo &info);
  [[nodiscard]] Flow emit_coprocessor();
  [[nodiscard]] Flow emit_other(const OpInfo &info);
  void build_interwork_key(unsigned value);
  void build_thumb_key(unsigned value);
  void write_pc(unsigned value, bool interwork);
  void write_pc(unsigned value) { write_pc(value, !thumb_); }
  [[nodiscard]] std::uint32_t link_value() const noexcept;
  [[nodiscard]] bool barrier_option_is_store() const noexcept;

  // Per-block state.
  Arm64Emitter emitter_;
  std::vector<Decoded> block_;
  std::vector<ColdPath> cold_;
  std::vector<TranslationColdPath> translation_cold_;
  std::vector<CrossingColdPath> crossing_cold_;
  std::vector<ResumePath> resume_cold_;
  std::vector<FloatColdPath> float_cold_;
  /* The index of the VMRS APSR_nzcv, FPSCR whose flags the host NZCV already
   * holds, because the VCMP before it left them there and nothing came
   * between. */
  std::size_t flags_held_by_vmrs_{~std::size_t{0}};
  /* A predicated instruction computes its result in a scratch register,
   * which done() merges into the destination with a CSEL on the host flags. */
  bool predicated_{false};
  unsigned predicate_condition_{};
  /* The compilation that learns a loop's bindings, the plan it recorded,
   * and the plan the translation of a loop runs on. */
  bool probing_{};
  /* The Neon floating-point operations compiled with a guard, and the
   * instructions that need the host FPCR to hold the guest's value, in the
   * translation so far (the loop probe's account of the body). */
  unsigned guarded_float_ops_{};
  unsigned guest_fp_mode_ops_{};
  /* How often a probing compilation touched each core register. */
  std::array<unsigned, 15> uses_{};
  /* The S shadows (s_use): the S register each host holds, or -1, with the
   * clock of its last use; the shadows the instruction being translated
   * reads; whether it may use them. */
  struct SShadow final {
    int s{-1};
    std::size_t used{};
  };
  /* The VFP scratch registers V3 and V4 hold sources only where an
   * instruction may not use shadows, V5 and V6 a result or an addend. */
  static constexpr std::array<unsigned, 6> s_shadow_hosts{0, 1, 2, 3, 4, 7};
  std::array<SShadow, s_shadow_hosts.size()> s_shadows_{};
  std::size_t s_shadow_clock_{};
  unsigned s_shadow_pinned_{};
  bool s_shadowing_{};
  bool looping_{};
  LoopPlan plan_{};
  Arm64Emitter::Label loop_head_{};
  std::size_t count_{};
  std::size_t units_consumed_{};
  std::uint64_t key_{};
  Isa isa_{Isa::a32};
  bool thumb_{};
  bool single_{};
  bool track_direct_stores_{};
  /* The translation keeps all fifteen core registers in the host: they are
   * loaded before the internal entry and written back only where the block
   * returns to the executor (JitCompilerFamilies::keep_gprs_across_links). */
  bool kept_gprs_{};
  bool terminated_{};
  bool native_fp_{};
  bool accesses_fp_state_{};
  /* The mode of the host FPCR where the instruction being translated
   * starts, as far as the translation can tell: it is unknown where the
   * translation begins and where two arms of a conditional body differ. */
  enum class FpMode : std::uint8_t { unknown, guest, standard };
  FpMode fp_mode_{FpMode::unknown};
  bool link_target_safe_{true};
  bool guest_illegal_{};
  std::uint64_t memory_write_mask_{};
  std::uint64_t direct_memory_write_mask_{};
  aarch32::Profile profile_;
};

} // namespace uml::cpu::arm64::jit::a32
