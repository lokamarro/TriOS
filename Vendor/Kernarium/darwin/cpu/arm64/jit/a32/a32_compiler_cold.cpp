// SPDX-License-Identifier: GPL-2.0-or-later
//
// The cold paths of a translation, emitted after its hot code: the exits a
// guard branches to, which report a fault or run an instruction the hot path
// declined.

#include "a32_compiler.hpp"
#include "a32_helpers.hpp"

namespace uml::cpu::arm64::jit::a32 {

void A32Compiler::emit_cold_paths() {
  for (const auto &path : translation_cold_)
    emit_translation_cold(path);
  for (const auto &path : crossing_cold_)
    emit_crossing_cold(path);
  for (const auto &path : resume_cold_) {
    bind(path.label);
    emit(0xd503'3f5fU); // clrex
    emitter_.branch(path.continuation);
  }
  for (const auto &path : float_cold_) {
    bind(path.label);
    emit_fp_write(true);
    emit_vec3(path.base, path.dest, path.a, path.b);
    emit_fp_write(false);
    emitter_.branch(path.continuation);
  }
  for (const auto &cold : cold_) {
    set_exit_gpr_mask(cold.dirty_gprs);
    set_exit_vector_mask(cold.dirty_vectors);
    bind(cold.label);
    /* Every edge leading here is flag-neutral: publish the translation-time
     * snapshot on this arm only, then let the exit consume CpuState. */
    publish_cold_flags(cold.flags);
    assume_flags_in_memory();
    count_ = cold.index;
    const auto &d = cur();
    switch (cold.kind) {
    case ColdPath::Kind::memory_fault:
      exit_event(Event::memory_fault, key_of(d), d.it);
      break;
    case ColdPath::Kind::illegal:
      exit_event(Event::illegal_instruction, key_of(d), d.it);
      break;
    case ColdPath::Kind::branch:
      if (cold.monitor) {
        emit(0xd503'3f5fU); // clrex
        clear_exclusive_monitor();
      }
      exit_direct(cold.target, 0U, cold.index + 1U);
      break;
    case ColdPath::Kind::code_changed:
      exit_event(Event::continued, next_key(), d.inst.next_it.bits());
      break;
    case ColdPath::Kind::slow:
      emit_slow_helper(cold);
      break;
    case ColdPath::Kind::exclusive_slow:
      /* A block that publishes direct stores holds a publication for its
       * whole invocation, and the serialized exclusive helper waits for
       * every other vCPU's: two vCPUs could wait on each other. It runs
       * after the executor ends this block's publication instead. */
      if (direct_memory_write_mask_ != 0U)
        emit_deferred_exclusive(cold);
      else
        emit_slow_helper(cold);
      break;
    case ColdPath::Kind::exclusive_mismatch:
      emit_slow_helper(cold);
      break;
    }
  }
}

void A32Compiler::emit_slow_helper(const ColdPath &cold) {
  /* The helper restores the host's own FPCR and FPSR before it runs. */
  const auto &d = cur();
  store_pc(key_of(d));
  spill_gprs_for_terminal_call();
  emitter_.move_x(0, 15);
  emitter_.move_x(1, 14);
  emitter_.load_constant(2, d.inst.word);
  emitter_.load_constant(3, key_of(d));
  emitter_.load_constant(4, d.it);
  emitter_.call(reinterpret_cast<const void *>(&uml_arm64_jit_a32_execute));
  emitter_.return_dynamic(cold.index + 1U);
}

void A32Compiler::emit_deferred_exclusive(const ColdPath &cold) {
  const auto &d = cur();
  store_it(d.it);
  store_pc(key_of(d));
  emitter_.load_constant(0, d.inst.word);
  emitter_.store_w(0, 19, offsetof(JitChainState, deferred_exclusive_instruction));
  /* The instruction at the key has not executed. */
  return_constant(Event::continued, cold.index);
}

} // namespace uml::cpu::arm64::jit::a32
