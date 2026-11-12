// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_compiler_families.hpp"
#include "jit_emitter.hpp"
#include "jit_runtime_contract.hpp"
#include "jit_semantic_memory.hpp"
#include "jit_sme_transition.hpp"

#include "cpu/arm64/detail/sme_encoding_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace uml::cpu::arm64 {

namespace detail {
struct Sm0Spill;
} // namespace detail

using ::uml::memory::Access;

class JitCompiler final : private jit::JitCompilerFamilies {
public:
  explicit JitCompiler(void *link_context = nullptr,
                       LinkResolver link_resolver = nullptr,
                       bool track_direct_stores = false,
                       IndirectLinkResolver indirect_link_resolver =
                           nullptr) noexcept;

  explicit JitCompiler(const ArmFeatureSet &translation_features) noexcept;

  void set_table_list_planning_enabled(bool enabled) noexcept;

  /* Compiles for the translation mode `mode`: the guest SVCR mode
   * (svcr_sm, svcr_za) and translation_mode_tbi, the mode the translation's
   * key carries (translation_key()), and so the mode its code runs in.
   * Mode 0, the default, is the translation without SME state whose data
   * addresses keep their top byte. */
  void set_translation_mode(std::uint8_t mode) noexcept;

  struct Result final {
    std::span<const std::uint32_t> words;
    /* The executable prefix of words, before the literal pool. */
    std::size_t executable_words{};
    /* Where this translation emitted executor-owned link operands. Shared
     * publication zeros those cells and retains these neutral guest keys so
     * another executor can resolve its own direct-link records and PIC sites.
     * Only a fully recorded translation may be reused.
     */
    std::span<const JitRelocation> relocations;
    bool relocatable{};
    /* Configured link resolvers can refuse stable direct or indirect records
     * when their bounded tables are full. Such code remains correct through
     * the ordinary C++ fallback, but is a degraded private result rather than
     * a canonical cross-executor translation.
     */
    std::size_t direct_link_refusals{};
    std::size_t indirect_link_refusals{};
    std::size_t guest_instructions{};
    std::size_t hot_words{};
    std::size_t internal_entry_word{};
    std::uint64_t memory_write_mask{};
    std::uint64_t direct_memory_write_mask{};
    bool accesses_fp_state{};
    bool link_target_safe{true};
    bool native_fp{};
    bool guest_illegal{};
  };

  Result compile(std::uint64_t guest_pc,
                 std::span<const std::uint32_t> instructions);

  Result compile_once(std::uint64_t guest_pc,
                      std::span<const std::uint32_t> instructions);

private:
  enum class Action {
    keep_going,
    terminate,
    unsupported,
  };

  void select_constrained_vector_pins(
      std::span<const std::uint32_t> instructions) noexcept;

  void select_consecutive_vector_lists(
      std::span<const std::uint32_t> instructions) noexcept;

  [[nodiscard]] bool
  translation_supports(ArmFeature feature) const noexcept;

  struct ColdPath final {
    enum class Action {
      memory_fault,
      pair_slow,
      simd_structure_slow,
      alignment_fault,
      ordered_slow,
      exclusive_slow,
      deferred_exclusive_slow,
      exclusive_compare_mismatch,
      memory_slow,
      memory_copy_set_slow,
      sme_slow,
      sm0_spill_slow,
      /* An ISB found the address space's code changed: leave at its
       * fall-through, which the executor fetches anew. */
      code_changed,
    };

    Arm64Emitter::Label label;
    std::uint32_t instruction;
    std::uint64_t pc;
    std::size_t count;
    Action action;
    std::uint16_t dirty_gprs{};
    std::uint32_t dirty_vectors{};
    GuestFlagsState flags{GuestFlagsState::memory};
  };

  struct TranslationColdPath final {
    Arm64Emitter::Label label;
    Arm64Emitter::Label continuation;
    Arm64Emitter::Label fault;
    unsigned address;
    Access access;
    /* A miss in both TLB ways takes the preserving miss bridge, or, when
     * false, branches to fault with every scratch register intact. */
    bool bridge{true};
  };

  /* A memory run whose accessed bytes straddle two pages at run time. Its
   * cold path translates both pages and replays the run's hot accesses,
   * each on the base for its page, then rejoins the hot path. */
  struct RunStraddleAccess final {
    std::size_t word{};
    /* The access's bytes, relative to the run's first accessed byte. */
    std::uint64_t start{};
    std::uint64_t end{};
  };

  struct RunStraddle final {
    static constexpr std::size_t maximum_accesses = 8;
    Arm64Emitter::Label label{};
    Arm64Emitter::Label join{};
    Arm64Emitter::Label slow{};
    Access access{};
    std::uint64_t first{};
    std::size_t words_begin{};
    std::size_t words_end{};
    std::array<RunStraddleAccess, maximum_accesses> accesses{};
    std::size_t count{};
  };

  class ColdPaths final {
  public:
    explicit ColdPaths(JitCompiler *compiler) noexcept
        : compiler_{compiler} {}

    void push_back(ColdPath path) {
      /*
       * Cold labels are emitted after the complete hot block, when later
       * guest instructions may have changed the compiler's flag state. Keep
       * the state at the edge with the label; flag-neutral guards allow the
       * rare arm to publish it without adding work to the hot path.
       */
      path.flags = compiler_->snapshot_flags();
      path.dirty_gprs = compiler_->dirty_gpr_mask();
      path.dirty_vectors = compiler_->dirty_vector_mask();
      paths_.push_back(path);
    }

    [[nodiscard]] std::vector<ColdPath>::const_iterator
    begin() const noexcept {
      return paths_.begin();
    }
    [[nodiscard]] std::vector<ColdPath>::const_iterator
    end() const noexcept {
      return paths_.end();
    }

  private:
    JitCompiler *compiler_;
    std::vector<ColdPath> paths_;
  };

  [[nodiscard]] static bool
  reservation_sensitive_allocation(std::uint32_t instruction) noexcept;

  void mark_memory_write() noexcept;

  void mark_direct_memory_write() noexcept;

  /* As above for each of the length instructions starting at count_. */
  void mark_direct_memory_writes(std::size_t length) noexcept;

  /* Makes the label of a cold exit for instruction at pc and records the
   * exit, retiring count_ + 1 instructions. Cold paths are emitted in the
   * order they are recorded. */
  [[nodiscard]] Arm64Emitter::Label cold_exit(std::uint32_t instruction,
                                              std::uint64_t pc,
                                              ColdPath::Action action);

  void track_direct_store(unsigned address, std::size_t size);

  void track_direct_store(unsigned address, std::size_t size,
                          std::size_t instruction_index);

  /* As above, with the stored size in a register. */
  void track_direct_store_register(unsigned address, unsigned size,
                                   std::size_t instruction_index);

  [[nodiscard]] bool emit_primary_hint(std::uint32_t instruction);

  void mask_register(unsigned reg, std::uint64_t mask, bool wide);

  [[nodiscard]] bool emit_system_register(std::uint32_t instruction);

  void emit_cache_zero(std::uint32_t instruction, std::uint64_t pc);

  void emit_code_synchronize(std::uint32_t instruction, std::uint64_t pc,
                             std::size_t count);

  void emit_code_change_check(std::uint32_t instruction, std::uint64_t pc);

  void emit_deferred_exclusive_slow(std::uint32_t instruction,
                                    std::uint64_t pc, std::size_t count);

  /* Moves 1 to 63 bytes between host pointers, from the given start and
   * end registers, then branches to done. A fill uses v0's low byte. */
  void emit_small_copy_set(bool set, unsigned source_start,
                           unsigned source_end, unsigned destination_start,
                           unsigned destination_end, unsigned remaining,
                           Arm64Emitter::Label done);
  /* A terminal FEAT_MOPS helper call for one stage, or for a whole
   * prologue/main/epilogue sequence when stages is 3. */
  void emit_memory_copy_set(std::uint32_t instruction, std::uint64_t pc,
                            std::size_t count, unsigned stages);

  [[nodiscard]] std::size_t
  try_emit_memory_copy_set_sequence(
      std::span<const std::uint32_t> instructions, std::uint64_t pc);

  /* The SME control core (jit_compiler_sme.cpp), on a host whose profile
   * implements SME. A guest write of SVCR, an SME slow path, and the ZERO
   * ZA of an SM=0 translation are terminal helper calls.
   * emit_sme_system() takes SMSTART, SMSTOP, MRS and MSR SVCR and
   * TPIDR2_EL0, and nothing for any other word. */
  [[nodiscard]] std::optional<Action>
  emit_sme_system(std::uint32_t instruction, std::uint64_t pc);
  /* A word of the SVE or SME encoding space. */
  [[nodiscard]] Action emit_sme_instruction(std::uint32_t instruction,
                                            std::uint64_t pc);
  void emit_sme_vector_length(std::uint32_t instruction, bool read);
  void emit_sme_array_vector(std::uint32_t instruction, std::uint64_t pc,
                             bool load);
  void emit_sme_zt0_transfer(std::uint32_t instruction, std::uint64_t pc,
                             bool load);
  /* x9 = the register file's ZT0, from CpuState and the run-time SVL. */
  void emit_sme_zt0_address();
  /* A register-only word an SM=1 translation runs natively: the
   * guest word with its general-register fields re-encoded to host
   * registers, and what it needs around it (jit_compiler_sme.cpp). */
  struct StreamingWord final {
    struct Gpr final {
      std::uint8_t low;
      std::uint8_t guest;
      bool stack_pointer;
      bool destination;
    };
    enum class Saved : std::uint8_t { none, z, p };
    std::uint32_t word{};
    std::array<Gpr, 3> gprs{};
    std::uint8_t gpr_count{};
    /* A ZA slice index Wv, one of W12-W15, which runs as W12. */
    bool slice{};
    std::uint8_t slice_low{};
    std::uint8_t slice_width{};
    std::uint8_t slice_guest{};
    bool reads_flags{};
    bool writes_flags{};
    /* It can raise floating-point exceptions; a rerun needs `saved`. */
    bool raises{};
    Saved saved{Saved::none};
    std::uint8_t saved_register{};
  };
  [[nodiscard]] static std::optional<StreamingWord>
  plan_sme_row(const detail::SmeEncoding &row, std::uint32_t instruction);
  /* An FP&SIMD data-processing word streaming mode runs: the classes the
   * shared table marks FP-legal. */
  [[nodiscard]] static std::optional<StreamingWord>
  plan_streaming_fp_simd(std::uint32_t instruction);
  void emit_streaming_word(const StreamingWord &plan);
  void emit_movprfx(std::uint32_t instruction, bool predicated);
  /* The SVE and SME1 memory forms in a streaming translation. */
  [[nodiscard]] bool emit_streaming_memory(const detail::SmeEncoding &row,
                                           std::uint32_t instruction,
                                           std::uint64_t pc);
  void emit_memory_span(const detail::SmeMemory &memory,
                        std::uint32_t instruction, unsigned destination);
  [[nodiscard]] bool emit_streaming_fp_memory(std::uint32_t instruction,
                                              std::uint64_t pc);
  void emit_terminal_helper(const void *helper, std::uint32_t instruction,
                            std::uint64_t pc, std::size_t count);

  void emit_simd_modified_immediate(std::uint32_t instruction,
                                    bool reads_destination);

  void emit_simd_duplicate_general(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_insert(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_duplicate_element(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_bit_unary(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_integer_reduce(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_scalar_pairwise_add(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_shift_immediate(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_shift_left_long(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_general_move(std::uint32_t instruction);

  void emit_translate(unsigned address, Access access,
                      Arm64Emitter::Label fallback, Arm64Emitter::Label fault,
                      std::size_t bytes, bool bridge = true);

  void emit_translation_cold(const TranslationColdPath &path);

  /* Strips the top byte of the final data address in `address`, as a
   * translation compiled for translation_mode_tbi does for every base,
   * SP included; it emits nothing otherwise. The address is the one the
   * access reaches, after any displacement, so a fault reports the untagged
   * address as the semantic path does. Flag-neutral. */
  void emit_untag_data_address(unsigned address);

  void emit_misalignment_check(unsigned address, std::size_t alignment,
                               Arm64Emitter::Label misaligned);

  /* Ends the software monitor at the end of a fused pair. */
  void clear_local_exclusive_monitor();

  [[nodiscard]] std::size_t
  try_emit_native_exclusive_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc);

  [[nodiscard]] Action emit_exclusive_memory(std::uint32_t instruction,
                                             std::uint64_t pc);

  [[nodiscard]] Action emit_atomic_memory(std::uint32_t instruction,
                                          std::uint64_t pc);

  void emit_native_scalar_atomic(std::uint32_t instruction,
                                 std::uint64_t pc);

  void emit_native_scalar_compare_swap(std::uint32_t instruction,
                                       std::uint64_t pc);

  /* The shared address step of the two above: x8 holds the translated,
   * write-checked base, with exclusive-slow and memory-fault exits. */
  void emit_native_atomic_address(std::uint32_t instruction,
                                  std::uint64_t pc);

  /* LDAR or LDAPR as the host's own load-acquire through the translated
   * base. A misaligned address takes `slow`. */
  void emit_native_ordered_load(std::uint32_t instruction, std::uint64_t pc,
                                ColdPath::Action slow);

  [[nodiscard]] bool emit_pair_memory(std::uint32_t instruction,
                                      std::uint64_t pc);

  [[nodiscard]] bool emit_simd_structure_memory(std::uint32_t instruction,
                                                std::uint64_t pc, bool single);

  [[nodiscard]] bool emit_other_memory(std::uint32_t instruction,
                                       std::uint64_t pc);

  /* A spill word of the SM = 0 vector-length family
   * (detail/sm0_vector_length.hpp). */
  void emit_sm0_spill(const detail::Sm0Spill &spill, std::uint32_t instruction,
                      std::uint64_t pc);

  [[nodiscard]] bool emit_literal_memory(std::uint32_t instruction,
                                         std::uint64_t pc);

  struct UnsignedMemoryOperation final {
    std::uint32_t instruction{};
    std::uint64_t offset{};
    std::size_t bytes{};
    unsigned base{};
    unsigned transfer{};
    bool load{};
  };

  [[nodiscard]] static bool decode_simd_unsigned_memory(
      std::uint32_t instruction,
      UnsignedMemoryOperation &operation) noexcept;

  [[nodiscard]] std::size_t try_emit_simd_unsigned_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc);

  /* A scalar unsigned-offset load or store whose base is SP exactly when
   * sp_base is set. */
  [[nodiscard]] static bool decode_scalar_unsigned_memory(
      std::uint32_t instruction, bool sp_base,
      UnsignedMemoryOperation &operation) noexcept;

  /* Emits each operation on the translated base in x8, naming its
   * transfer register's native binding, and records the accesses for the
   * run's straddle path, which `proved` says the run has. */
  void emit_native_gpr_run(std::span<const UnsignedMemoryOperation> operations,
                           bool load, RunStraddle &straddle, bool proved);

  /* Moves a run's base register forward or back by an offset. */
  void move_run_base(std::uint64_t offset, bool forward, unsigned reg = 8U);

  /* Advances x8 from a run's guest base to the first byte the run
   * accesses, base + first, and branches to `outside` unless the bytes up
   * to base + end lie in that byte's page. It strips the top byte of that
   * first address first (emit_untag_data_address()). Returns whether it
   * emitted that branch. */
  bool emit_run_span_proof(std::uint64_t first, std::uint64_t end,
                           Arm64Emitter::Label outside);

  /* The TLB set for the guest address in `address` in x0 and its page
   * tag in x1, as emit_translate() computes them. */
  void emit_tlb_set(unsigned address);

  /* Records the native access emitted next as one of the straddle's. */
  void note_run_straddle_access(RunStraddle &straddle, std::uint64_t offset,
                                std::size_t bytes);

  void emit_run_straddle(const RunStraddle &straddle);

  [[nodiscard]] std::size_t try_emit_sp_unsigned_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc);

  [[nodiscard]] std::size_t try_emit_gpr_unsigned_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc);

  [[nodiscard]] bool emit_unsigned_memory(std::uint32_t instruction,
                                          std::uint64_t pc);

  /*
   * These ordinary control-flow classes terminate almost every translated
   * block and do not overlap the system, ordered-memory, or extension
   * allocations handled below. Resolve them immediately after architectural
   * exception instructions so cold translation does not walk the complete
   * decoder for a branch already known to end the block.
   */
  bool emit_common_control_flow(std::uint32_t instruction, std::uint64_t pc,
                                std::size_t resulting_count);

  Action emit_instruction(std::uint32_t instruction, std::uint64_t pc);

  Arm64Emitter emitter_;
  ColdPaths cold_;
  std::vector<TranslationColdPath> translation_cold_;
  std::vector<RunStraddle> run_straddles_;
  std::size_t count_{};
  std::uint64_t memory_write_mask_{};
  std::uint64_t direct_memory_write_mask_{};
  bool terminated_{};
  bool accesses_fp_state_{};
  bool link_target_safe_{true};
  bool native_fp_{};
  bool guest_illegal_{};
  void *configured_link_context_{};
  LinkResolver configured_link_resolver_{};
  IndirectLinkResolver configured_indirect_link_resolver_{};
  bool track_direct_stores_{};
  bool table_list_planning_enabled_{true};
  const ArmFeatureSet *translation_features_{};
};

using JitFunction = std::uint64_t (*)(CpuState *, Tlb *, JitChainState *);

} // namespace uml::cpu::arm64
