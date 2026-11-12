// SPDX-License-Identifier: GPL-2.0-or-later

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_operations.hpp"

#include "cpu/arm64/jit/jit_shared_translations.hpp"

#include <algorithm>
#include <cstring>

namespace uml::cpu::arm64::jit::a32 {

using aarch32::Isa;
using aarch32::ItState;
using aarch32::Status;

namespace {

/* A T32 block of this many instructions ends at the next boundary outside an
 * IT block. The ceiling leaves room for the four instructions of an IT block
 * opened by the last of them, so no block exceeds maximum_block_instructions
 * (the write masks are indexed by instruction). A32 has no IT blocks, so an
 * A32 block takes all of them. */
constexpr std::size_t t32_block_target = maximum_block_instructions - 4U;

[[nodiscard]] std::uint32_t load16(std::span<const std::byte> bytes,
                                   std::size_t offset) noexcept {
  std::uint16_t value;
  std::memcpy(&value, bytes.data() + offset, sizeof value);
  return value;
}

[[nodiscard]] std::uint32_t load32(std::span<const std::byte> bytes,
                                   std::size_t offset) noexcept {
  std::uint32_t value;
  std::memcpy(&value, bytes.data() + offset, sizeof value);
  return value;
}

} // namespace

A32Compiler::A32Compiler(void *link_context, LinkResolver link_resolver,
                         bool track_direct_stores,
                         IndirectLinkResolver indirect_link_resolver) noexcept
    : emitter_{std::size_t{1} << 18U},
      track_direct_stores_{track_direct_stores},
      profile_{decode_profile()} {
  attach_family_emitter(&emitter_, &native_fp_, !track_direct_stores_);
  /* The guest has fifteen core registers, and the host has the registers
   * for all of them. */
  gpr_host_limit_ = resident_gpr_hosts.size();
  emitter_.set_extended_frame(true);
  /* Translations that link keep the registers in the host from block to
   * block; an unlinked one (the tracked-store variant, the single
   * instruction) is entered from and returns to the executor. */
  kept_gprs_ = link_resolver != nullptr && !track_direct_stores_;
  if (kept_gprs_)
    keep_gprs_across_links();
  attach_link_resolver(link_context, link_resolver);
  attach_indirect_link_resolver(link_context, indirect_link_resolver);
  emitter_.attach_exit_flush(this, [](void *context) {
    auto *self = static_cast<A32Compiler *>(context);
    self->flush_exit_gprs();
  });
  emitter_.attach_flag_clobber(this, [](void *context) {
    static_cast<A32Compiler *>(context)->spill_flags_for_internal_clobber();
  });
}

std::uint64_t A32Compiler::key_at(std::uint32_t pc) const noexcept {
  return thumb_ ? aarch32::t32_key(pc) : aarch32::a32_key(pc);
}

void A32Compiler::decode_block(const Request &request) {
  block_.clear();
  const auto bytes = request.bytes;
  ItState it = request.itstate;
  std::size_t offset = 0;
  std::uint32_t pc = aarch32::key_pc(request.key);
  const std::size_t unit = thumb_ ? 2U : 4U;
  const std::size_t target =
      thumb_ ? t32_block_target : maximum_block_instructions;
  while (true) {
    if (single_ && !block_.empty())
      break;
    if (block_.size() >= target && !it.active())
      break;
    if (offset + unit > bytes.size())
      break;
    Decoded d;
    d.pc = pc;
    d.it = it.bits();
    if (thumb_) {
      const auto hw1 = load16(bytes, offset);
      const auto length = aarch32::thumb_length(static_cast<std::uint16_t>(hw1));
      if (offset + length > bytes.size())
        break;
      const auto word =
          length == 4U ? (hw1 << 16) | load16(bytes, offset + 2U) : hw1 << 16;
      d.inst = aarch32::decode_thumb(word, it, profile_);
    } else {
      d.inst = aarch32::decode_a32(load32(bytes, offset), profile_);
    }
    it = d.inst.next_it;
    const auto length = d.inst.length;
    block_.push_back(d);
    offset += length;
    pc += length;
    if (ends_block(d.inst) && !continues_through(d))
      break;
  }
}

Result A32Compiler::compile(const Request &request) {
  begin_link_observation();
  key_ = request.key;
  thumb_ = aarch32::key_is_thumb(key_);
  isa_ = thumb_ ? Isa::t32 : Isa::a32;
  single_ = request.single;
  accesses_fp_state_ = false;
  guarded_float_ops_ = 0;
  guest_fp_mode_ops_ = 0;
  fp_mode_ = FpMode::unknown;
  flags_held_by_vmrs_ = ~std::size_t{0};
  decode_block(request);
  Result result;
  if (block_.empty())
    return result;
  looping_ = false;
  if (!probing_ && loop_candidate(request)) {
    A32Compiler probe{nullptr, nullptr, false, nullptr};
    probe.probing_ = true;
    (void)probe.compile(request);
    if (probe.plan_.valid) {
      plan_ = probe.plan_;
      looping_ = true;
    }
  }
  emitter_.prologue();
  if (kept_gprs_)
    load_kept_gprs();
  const auto internal_entry_word = emitter_.size();
  emit_block();
  const auto instructions = count_;
  const auto hot_words = emitter_.size();
  emit_cold_paths();
  const auto words = emitter_.finish();
  auto &translation = result.translation;
  translation.words = words;
  translation.executable_words = emitter_.executable_words();
  translation.relocations = emitter_.relocations();
  translation.relocatable = emitter_.relocatable() &&
                            emitter_.relocations().size() <=
                                maximum_shared_translation_relocations;
  translation.direct_link_refusals = direct_link_refusals();
  translation.indirect_link_refusals = indirect_link_refusals();
  translation.guest_instructions = instructions;
  translation.hot_words = hot_words;
  translation.internal_entry_word = internal_entry_word;
  translation.memory_write_mask = memory_write_mask_;
  translation.direct_memory_write_mask = direct_memory_write_mask_;
  translation.accesses_fp_state = accesses_fp_state_;
  translation.link_target_safe = link_target_safe_;
  translation.native_fp = native_fp_;
  translation.guest_illegal = guest_illegal_;
  result.guest_units = units_consumed_;
  return result;
}

/* The fifteen core registers, two to a load, in the host registers of their
 * bindings. A linked predecessor leaves them there; the executor leaves them
 * in the CpuState. */
void A32Compiler::load_kept_gprs() {
  gpr_slots_ = {};
  for (unsigned guest = 0; guest < 15U; ++guest)
    gpr_slots_[guest] = {.guest = guest, .stack_pointer = false,
                         .loaded = true, .dirty = false};
  for (unsigned guest = 0; guest + 1U < 15U; guest += 2U)
    emitter_.load_pair_x(resident_gpr_hosts[guest],
                         resident_gpr_hosts[guest + 1U], 15,
                         register_offset(guest, false));
  emitter_.load_x(resident_gpr_hosts[14], 15, register_offset(14U, false));
}

/* The last instruction is a backward B to the translation's own first
 * instruction, and nothing in the block needs a boundary of its own. */
bool A32Compiler::loop_candidate(const Request &request) {
  if (single_ || track_direct_stores_ || request.itstate.bits() != 0U ||
      block_.size() < 2U)
    return false;
  for (const auto &d : block_)
    if (d.inst.status != Status::ok || is_exclusive_op(op_info(d.inst.form).op))
      return false;
  const auto &last = block_.back();
  if (last.it != 0U || last.inst.next_it.bits() != 0U ||
      op_info(last.inst.form).op != Op::b ||
      static_cast<std::int32_t>(last.inst.operands.imm32) >= 0)
    return false;
  const auto saved = count_;
  count_ = block_.size() - 1U;
  const std::uint32_t target =
      pc_read() + static_cast<std::uint32_t>(last.inst.operands.imm32);
  count_ = saved;
  return (thumb_ ? aarch32::t32_key(target & ~1U)
                 : aarch32::a32_key(target & ~3U)) == request.key;
}

void A32Compiler::record_loop_plan() {
  plan_.valid = true;
  plan_.standard_fp = guarded_float_ops_ != 0U && guest_fp_mode_ops_ == 0U;
  /* The registers the loop touches most hold the host registers, not the
   * first ones it names. */
  std::array<unsigned, 15> order{};
  for (unsigned r = 0; r < 15U; ++r)
    order[r] = r;
  std::stable_sort(order.begin(), order.end(),
                   [&](unsigned a, unsigned b) { return uses_[a] > uses_[b]; });
  plan_.gpr.fill(unbound_guest);
  for (std::size_t index = 0; index < plan_.gpr.size() && uses_[order[index]] != 0U;
       ++index)
    plan_.gpr[index] = order[index];
  for (std::size_t index = 0; index < vector_slots_.size(); ++index)
    plan_.vector[index] = vector_slots_[index].guest;
}

/* The registers the loop holds are loaded and marked dirty before the head,
 * so every exit from the loop writes them back and a back edge writes
 * nothing. */
void A32Compiler::apply_loop_plan() {
  for (std::size_t index = 0; index < gpr_slots_.size(); ++index) {
    const unsigned guest = plan_.gpr[index];
    if (guest == unbound_guest || kept_gprs_)
      continue;
    gpr_slots_[index] = {.guest = guest, .stack_pointer = false,
                         .loaded = true, .dirty = true};
    emitter_.load_x(resident_gpr_hosts[index], 15, register_offset(guest, false));
  }
  for (std::size_t index = 0; index < vector_slots_.size(); ++index) {
    const unsigned guest = plan_.vector[index];
    if (guest == unbound_guest)
      continue;
    vector_slots_[index] = {.guest = guest, .loaded = true, .dirty = true};
    emitter_.load_q(resident_vector_hosts[index], 15, vector_slot_offset(guest));
  }
  /* A loop of guarded Neon operations switches the host to the Standard
   * FPCR once, here, and runs them unguarded; a VFP instruction after the
   * loop switches it back. */
  if (plan_.standard_fp && !probing_) {
    touch_fp_environment();
    set_fp_mode(true);
  }
  loop_head_ = make_label();
  bind(loop_head_);
}

/* The back edge: while the budget lasts the loop goes round, and the other
 * arm leaves as a branch does. */
A32Compiler::Flow A32Compiler::emit_loop_branch() {
  const auto &d = cur();
  const unsigned condition = d.inst.cond;
  const std::size_t iteration = block_.size();
  Arm64Emitter::Label skip{};
  if (condition < 14U) {
    load_flags();
    synchronize_flags();
    skip = make_label();
    branch_condition(skip, condition ^ 1U);
  } else {
    synchronize_flags();
  }
  /* A linked successor needs the budget of its own length after this
   * block's; a loop is its own successor. */
  const auto fallback = make_label();
  emit(0xf100'001fU | static_cast<std::uint32_t>(2U * iteration) << 10 |
       20U << 5); // cmp x20, #(2 * iteration)
  branch_condition(fallback, 3); // LO
  emitter_.subtract_immediate(20, 20, static_cast<unsigned>(iteration));
  emitter_.branch(loop_head_);
  bind(fallback);
  exit_continue_at(key_, 0U, iteration);
  if (condition < 14U) {
    bind(skip);
    exit_direct(next_key(), d.inst.next_it.bits(), iteration);
  }
  return Flow::end;
}

void A32Compiler::emit_block() {
  count_ = 0;
  terminated_ = false;
  forget_s_shadows();
  if (looping_)
    apply_loop_plan();
  while (count_ < block_.size()) {
    const auto flow = emit_instruction();
    if (flow == Flow::split) {
      /* The exit before the instruction is emitted; count_ is the number of
       * instructions before it. */
      terminated_ = true;
      break;
    }
    ++count_;
    if (flow == Flow::end) {
      terminated_ = true;
      break;
    }
  }
  /* count_ is the number of instructions the translation holds. */
  units_consumed_ = 0;
  for (std::size_t index = 0; index < count_; ++index)
    units_consumed_ += block_[index].inst.length / (thumb_ ? 2U : 4U);
  if (!terminated_) {
    /* The fetch window cut the block short. Its successor is as static as a
     * branch target, so the exit asks for a link as a branch's does; an
     * unlinked one returns to the executor after every full window. */
    const auto &last = block_[count_ - 1U];
    exit_direct(key_at(last.pc + last.inst.length), last.inst.next_it.bits(),
                count_);
  }
}

A32Compiler::Flow A32Compiler::emit_instruction() {
  const auto &d = cur();
  const auto &inst = d.inst;
  begin_s_shadows();
  if (count_ + 1U == block_.size()) {
    if (probing_)
      record_loop_plan();
    if (looping_)
      return emit_loop_branch();
  }
  if (inst.status == Status::undefined) {
    exit_illegal();
    return Flow::end;
  }
  /* A reservation-establishing helper waits for every other vCPU's published
   * store block, so it cannot run inside a publication an earlier
   * instruction or a linked predecessor began: such an instruction opens its
   * own block, which no predecessor chains into (as the A64 compiler does). */
  if (is_exclusive_op(op_info(inst.form).op)) {
    if (count_ != 0U) {
      exit_continue_at(key_of(d), d.it, count_);
      return Flow::split;
    }
    link_target_safe_ = false;
    /* A load-exclusive that opens the block may fuse with its store. */
    if (const auto consumed = try_fuse_exclusive(); consumed != 0U) {
      count_ = consumed - 1U;
      return Flow::next;
    }
  }
  const unsigned condition = inst.cond;
  if (count_ + 1U < block_.size() && continues_through(d))
    return emit_side_exit_branch();
  if (inst.status == Status::nop || condition >= 14U)
    return emit_operation();
  return emit_guarded(condition);
}

/* A conditional branch back is as likely to be a `continue` that is rarely
 * taken (Horizon Detection's bounds tests, each its own block of two to four
 * instructions) as the end of a loop, and the side exit costs the link of the
 * taken path either way. A branch back to the first instruction ends the
 * block: the host loop is better than a side exit that links to itself. */
bool A32Compiler::continues_through(const Decoded &d) const {
  if (continues_past(d.inst))
    return true;
  const auto &inst = d.inst;
  if (inst.status != Status::ok || inst.next_it.bits() != 0U ||
      inst.cond >= 14U || op_info(inst.form).op != Op::b)
    return false;
  const std::uint32_t target = d.pc + (thumb_ ? 4U : 8U) +
                               static_cast<std::uint32_t>(inst.operands.imm32);
  return target != aarch32::key_pc(key_);
}

/* A conditional forward branch inside a block: the taken path leaves through
 * a cold side exit that publishes the registers dirty here and links to the
 * target, and the block goes on with the fall-through, its registers and
 * flags still in the host. */
A32Compiler::Flow A32Compiler::emit_side_exit_branch(bool monitor) {
  const auto &inst = cur().inst;
  const auto &o = inst.operands;
  const auto info = op_info(inst.form);
  const std::uint32_t target = pc_read() + static_cast<std::uint32_t>(o.imm32);
  if (info.op == Op::b) {
    load_flags();
    const auto side = cold_exit(ColdPath::Kind::branch);
    cold_.back().target = thumb_ ? aarch32::t32_key(target & ~1U)
                                 : aarch32::a32_key(target & ~3U);
    cold_.back().monitor = monitor;
    emitter_.branch_condition(side, inst.cond);
  } else {
    const unsigned n = use(static_cast<unsigned>(o.n), 9);
    const auto side = cold_exit(ColdPath::Kind::branch);
    cold_.back().target = aarch32::t32_key(target);
    cold_.back().monitor = monitor;
    emitter_.compare_zero(side, n, false, info.op == Op::cbnz);
  }
  return Flow::next;
}

bool A32Compiler::can_predicate() const {
  const auto &inst = cur().inst;
  const auto &o = inst.operands;
  const auto info = op_info(inst.form);
  switch (info.op) {
  case Op::adc: case Op::add: case Op::and_: case Op::bic: case Op::eor:
  case Op::mov: case Op::mvn: case Op::orn: case Op::orr: case Op::rsb:
  case Op::rsc: case Op::sbc: case Op::sub:
    // A shift by a register compares its amount and clobbers the host flags.
    return !o.setflags && o.d != 15 && info.shape != Shape::reg_shifted;
  case Op::movw:
    return o.d != 15;
  default:
    return false;
  }
}

A32Compiler::Flow A32Compiler::emit_predicated(unsigned condition) {
  /* The instruction runs on both arms with the guest's flags in the host
   * NZCV, which it leaves alone, and writes its result to a scratch
   * register; done() selects between that and the destination's old value. */
  load_flags();
  predicated_ = true;
  predicate_condition_ = condition;
  const auto flow = emit_operation();
  predicated_ = false;
  return flow;
}

A32Compiler::Flow A32Compiler::emit_guarded(unsigned condition) {
  if (can_predicate())
    return emit_predicated(condition);
  /* The body runs on one arm only. Publish the flags both arms keep, branch
   * on the host flags, and leave both arms with the same register bindings
   * (restore_residency/close_conditional_body). */
  load_flags();
  synchronize_flags();
  const auto mark = mark_residency();
  const auto mode_before = fp_mode_;
  const auto skip = make_label();
  branch_condition(skip, condition ^ 1U);
  const auto flow = emit_operation();
  if (flow == Flow::end) {
    restore_residency(mark);
    bind(skip);
    /* The skipped arm never ran the body: it holds the FPCR it had. */
    fp_mode_ = mode_before;
    const auto &d = cur();
    /* The instruction's condition failed: the block continues after it with
     * the IT state that follows it, to a static successor. */
    exit_direct(key_at(d.pc + d.inst.length), d.inst.next_it.bits(),
                count_ + 1U);
    return Flow::end;
  }
  /* The arms join with the FPCR they have, which the translation knows
   * only where they agree. */
  if (fp_mode_ != mode_before)
    fp_mode_ = FpMode::unknown;
  close_conditional_body(mark);
  bind(skip);
  return Flow::next;
}

A32Compiler::ResidencyMark A32Compiler::mark_residency() const {
  return {gpr_slots_, vector_slots_};
}

void A32Compiler::restore_residency(const ResidencyMark &mark) {
  gpr_slots_ = mark.gpr;
  vector_slots_ = mark.vector;
}

void A32Compiler::close_conditional_body(const ResidencyMark &mark) {
  /* A binding the body made holds a value that exists on one arm only. Write
   * it back there and forget it, so both arms continue as they began. */
  for (std::size_t index = 0; index < gpr_slots_.size(); ++index) {
    auto &slot = gpr_slots_[index];
    if (mark.gpr[index].guest != unbound_guest || slot.guest == unbound_guest)
      continue;
    if (slot.dirty)
      emitter_.store_x(resident_gpr_hosts[index], 15,
                       register_offset(slot.guest, false));
    slot = {};
  }
  for (std::size_t index = 0; index < vector_slots_.size(); ++index) {
    auto &slot = vector_slots_[index];
    if (mark.vector[index].guest != unbound_guest ||
        slot.guest == unbound_guest)
      continue;
    if (slot.dirty)
      emitter_.store_q(resident_vector_hosts[index], 15,
                       vector_slot_offset(slot.guest));
    slot = {};
  }
}

// ---------------------------------------------------------------------------
// Exits.

void A32Compiler::store_it(std::uint8_t value) {
  if (!single_ && value == 0U)
    return;
  emitter_.load_constant(8, value);
  emitter_.store_b(8, 15, offsetof(CpuState, a32_itstate));
}

void A32Compiler::exit_event(Event event, std::uint64_t pc_key,
                             std::uint8_t it) {
  store_it(it);
  store_pc(pc_key);
  return_constant(event, count_ + 1U);
}

void A32Compiler::exit_illegal() {
  guest_illegal_ = count_ == 0U;
  exit_event(Event::illegal_instruction, key_of(cur()), cur().it);
}

void A32Compiler::exit_continue_at(std::uint64_t pc_key, std::uint8_t it,
                                   std::size_t count) {
  store_it(it);
  store_pc(pc_key);
  return_constant(Event::continued, count);
}

void A32Compiler::exit_direct(std::uint64_t target_key) {
  exit_direct(target_key, cur().inst.next_it.bits(), count_ + 1U);
}

void A32Compiler::exit_direct(std::uint64_t target_key, std::uint8_t it,
                              std::size_t count) {
  if (it != 0U || single_) {
    /* A linked successor assumes an ITSTATE of zero, and a single
     * instruction runs unlinked. */
    exit_continue_at(target_key, it, count);
    return;
  }
  emit_linked_exit(target_key, count);
}

void A32Compiler::exit_indirect(unsigned key_register) {
  const auto it = cur().inst.next_it.bits();
  if (it != 0U || single_) {
    /* The key is stored before the ITSTATE, whose store loads its constant
     * into x8. */
    emitter_.store_x(key_register, 15, offsetof(CpuState, pc));
    store_it(it);
    return_constant(Event::continued, count_ + 1U);
    return;
  }
  emit_indirect_linked_exit(key_register, false, key_of(cur()), count_ + 1U,
                            true);
}

void A32Compiler::emit_code_change_check() {
  const auto changed = cold_exit(ColdPath::Kind::code_changed);
  emitter_.load_x(0, 15, offsetof(CpuState, code_sequence));
  emitter_.load_x(1, 15, offsetof(CpuState, code_sequence_seen));
  emitter_.load_x(0, 0, 0);
  emit(0xcb01'0000U); // sub x0, x0, x1
  compare_zero(changed, 0, true, true);
}

Arm64Emitter::Label A32Compiler::cold_exit(ColdPath::Kind kind) {
  const auto label = make_label();
  cold_.push_back({.label = label,
                   .kind = kind,
                   .index = count_,
                   .dirty_gprs = dirty_gpr_mask(),
                   .dirty_vectors = dirty_vector_mask(),
                   .flags = snapshot_flags()});
  return label;
}

} // namespace uml::cpu::arm64::jit::a32
