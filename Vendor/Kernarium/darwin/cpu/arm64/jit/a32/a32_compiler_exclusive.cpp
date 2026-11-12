// SPDX-License-Identifier: GPL-2.0-or-later
//
// LL/SC fusion for the AArch32 exclusives (the A64 compiler's
// try_emit_native_exclusive_run): a LDREX{B,H}, up to four register-only data
// processing instructions and a STREX{B,H} on the same address at the start
// of a block become one host load-exclusive, the instructions, and one host
// store-exclusive, so the guest's monitor is the host's. The guest address is
// translated for write and for read before the monitor is established and
// the registers the run needs are resident, so only register instructions
// execute between the host pair; a failed store leaves the status register to
// the guest's retry loop, exactly as the architecture allows.
//
// A STREX under a condition (STREXEQ after a CMP, the compare-and-swap
// idiom) branches over the store on the host flags the run's CMP left, and
// the skipped arm clears the monitor. A conditional forward branch or
// CBZ/CBNZ between the two (the other compare-and-swap and try-lock idiom,
// LDREX, CMP, BNE out, STREX) is a side exit whose taken arm clears the
// monitor and links to the target. Whatever the run declines (a pair, a
// condition on anything but the store, an instruction that is not plain
// register arithmetic, an IT block, too many live registers) takes the slow
// helper, one instruction at a time.

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_operations.hpp"

#include <array>
#include <cstddef>

namespace uml::cpu::arm64::jit::a32 {

using aarch32::Status;
using ::uml::memory::Access;

namespace {

constexpr unsigned host_address = 21;
/* Instructions between the load and the store. */
constexpr std::size_t maximum_interval = 4;

[[nodiscard]] unsigned size_field(unsigned bytes) noexcept {
  return bytes == 1U ? 0U : bytes == 2U ? 1U : 2U;
}

} // namespace

std::size_t A32Compiler::try_fuse_exclusive() {
  if (count_ != 0U || single_ || block_.size() < 2U ||
      !(gprs_kept_across_links() || gpr_slots_unbound()))
    return 0;
  const auto &first = block_[0];
  if (first.it != 0U || first.inst.cond < 14U)
    return 0;
  const auto load = describe_transfer(first.inst);
  if (load.kind != Transfer::Kind::exclusive || !load.load || load.bytes == 8U ||
      load.t == load.n || load.t == 15U || load.n == 15U)
    return 0;

  // Find the store and check what lies between.
  std::size_t store_index = 0;
  Transfer store;
  bool compare_seen = false;
  bool flags_first = false;
  bool leaves = false;
  for (std::size_t index = 1; index < block_.size() &&
                              index <= maximum_interval + 1U; ++index) {
    const auto &d = block_[index];
    const auto &inst = d.inst;
    if (d.it != 0U || inst.status != Status::ok)
      return 0;
    const auto info = op_info(inst.form);
    if (is_exclusive_op(info.op)) {
      store = describe_transfer(inst);
      if (store.load || store.bytes != load.bytes || store.n != load.n ||
          store.imm != load.imm || store.t == 15U || store.d == 15U)
        return 0;
      store_index = index;
      break;
    }
    // A register-only operation: no flag-setting but the compares, no
    // register-controlled shift, no carry input, no PC, and the base is
    // never rewritten.
    const auto &o = inst.operands;
    if (continues_past(inst)) {
      // The taken arm leaves the run: a B reads the flags, so a compare must
      // have set them in the run or they are loaded before it starts.
      leaves = true;
      if (info.op == Op::b && !compare_seen)
        flags_first = true;
      continue;
    }
    if (inst.cond < 14U)
      return 0;
    switch (info.op) {
    case Op::add: case Op::sub: case Op::and_: case Op::orr: case Op::eor:
    case Op::bic: case Op::orn: case Op::rsb: case Op::mov: case Op::mvn:
    case Op::cmp: case Op::cmn:
      break;
    default:
      return 0;
    }
    const bool compare = info.op == Op::cmp || info.op == Op::cmn;
    compare_seen = compare_seen || compare;
    if (info.shape != Shape::immediate && info.shape != Shape::reg)
      return 0;
    if (!compare && o.setflags)
      return 0;
    if (info.shape == Shape::reg && o.shift_t == aarch32::SRType::RRX)
      return 0;
    const unsigned n = info.sp_base ? 13U : static_cast<unsigned>(o.n);
    const bool moves = info.op == Op::mov || info.op == Op::mvn;
    if ((!moves && n == 15U) || (info.shape == Shape::reg && o.m == 15))
      return 0;
    if (!compare && (o.d == 15 || static_cast<unsigned>(o.d) == load.n))
      return 0;
  }
  if (store_index == 0U)
    return 0;
  const bool conditional = block_[store_index].inst.cond < 14U;

  // The registers the run keeps resident: all of them or none.
  std::array<bool, 16> needed{};
  std::size_t needed_count = 0;
  const auto need = [&](unsigned guest) {
    if (!needed[guest]) {
      needed[guest] = true;
      ++needed_count;
    }
  };
  need(load.n);
  need(load.t);
  need(store.t);
  need(store.d);
  for (std::size_t index = 1; index < store_index; ++index) {
    const auto &inst = block_[index].inst;
    const auto info = op_info(inst.form);
    const auto &o = inst.operands;
    if (continues_past(inst)) {
      if (info.op != Op::b)
        need(static_cast<unsigned>(o.n));
      continue;
    }
    const bool compare = info.op == Op::cmp || info.op == Op::cmn;
    const bool moves = info.op == Op::mov || info.op == Op::mvn;
    if (!moves)
      need(info.sp_base ? 13U : static_cast<unsigned>(o.n));
    if (info.shape == Shape::reg)
      need(static_cast<unsigned>(o.m));
    if (!compare)
      need(static_cast<unsigned>(o.d));
  }
  if (needed_count > gpr_slot_capacity())
    return 0;

  const unsigned bytes = load.bytes;
  const auto slow = cold_exit(ColdPath::Kind::exclusive_slow);
  const auto fault = cold_exit(ColdPath::Kind::memory_fault);

  const unsigned base = use(load.n, fallback_n);
  const auto form_address = [&] {
    if (load.imm != 0U)
      emit(enc::add_i(address_register, base, load.imm));
    else
      emit(enc::mov(address_register, base));
  };
  form_address();
  emit_alignment_check(address_register, bytes, slow);
  emit_translate(address_register, Access::write, slow, slow, bytes,
                 std::nullopt, static_cast<unsigned>(bytes));
  emit(enc::x::mov(host_address, address_register));
  // The store's range is recorded at the store's own index, before the
  // monitor is established.
  count_ = store_index;
  track_direct_store(host_address, bytes);
  count_ = 0;
  form_address();
  emit_translate(address_register, Access::read, slow, fault, bytes);
  emit(enc::x::cmp(address_register, host_address));
  emitter_.branch_condition(slow, enc::ne);

  // A conditional store or a branch reads the host flags: make them valid
  // before the monitor is established unless the run's own compare sets them.
  // The address check above sets the host flags, so this follows it.
  if (flags_first || (conditional && !compare_seen))
    load_flags();

  // Bind everything the interval reads before it starts.
  const auto preload = [&](unsigned guest) {
    (void)read_native_gpr(guest, false, fallback_m);
  };
  for (std::size_t index = 1; index < store_index; ++index) {
    const auto &inst = block_[index].inst;
    const auto info = op_info(inst.form);
    const auto &o = inst.operands;
    if (continues_past(inst)) {
      if (info.op != Op::b)
        preload(static_cast<unsigned>(o.n));
      continue;
    }
    const bool moves = info.op == Op::mov || info.op == Op::mvn;
    if (!moves)
      preload(info.sp_base ? 13U : static_cast<unsigned>(o.n));
    if (info.shape == Shape::reg)
      preload(static_cast<unsigned>(o.m));
  }
  const bool loaded_is_stored = store.t == load.t;
  if (!loaded_is_stored)
    preload(store.t);
  // A store that may be skipped, or a run that may be left before it,
  // keeps the status register's old value.
  const auto status =
      write_native_gpr(store.d, false, conditional || leaves, fallback_n);
  const auto loaded = write_native_gpr(load.t, false, false, fallback_n);
  // The interval: the host load, the instructions, the host store.
  emit(enc::ldxr(size_field(bytes), loaded.host, host_address, load.acquire));
  commit_native_gpr(loaded, load.t, false);
  link_target_safe_ = false;
  count_ = 1;
  for (; count_ < store_index; ++count_) {
    if (continues_past(cur().inst))
      (void)emit_side_exit_branch(true);
    else
      (void)emit_operation();
  }
  Arm64Emitter::Label resume{};
  if (conditional) {
    resume = make_label();
    const auto skipped = make_label();
    emitter_.branch_condition(skipped, block_[store_index].inst.cond ^ 1U);
    resume_cold_.push_back({.label = skipped, .continuation = resume});
  }
  mark_direct_memory_write();
  const auto source = read_native_gpr(store.t, false, fallback_m);
  emit(enc::stxr(size_field(bytes), status.host, source.host, host_address,
                 store.release));
  commit_native_gpr(status, store.d, false);
  if (conditional)
    bind(resume);
  clear_exclusive_monitor();
  emitter_.load_b(host_address, 19, offsetof(JitChainState, link_selector));
  // The caller's loop counts the last instruction of the run.
  return store_index + 1U;
}

} // namespace uml::cpu::arm64::jit::a32
