// SPDX-License-Identifier: GPL-2.0-or-later
//
// VFP and Advanced SIMD: the register views and the floating-point
// environment, and the dispatch to the families.
//
// Q0-Q15 are CpuState vectors 0-15, which the A64 compiler's vector
// residency binds to host registers V16-V31: a Q operand is its resident
// host register. D n is the low (even n) or high (odd n) 64 bits of Q n/2 and
// S n is the 32-bit lane n%4 of Q n/4, so an operand narrower than a Q is
// read by moving its lane into a scratch register and written by inserting
// the result into the lane: a scalar or 64-bit A64 operation zeroes the rest
// of its destination, which here is another register or lane.
//
// Floating point. A VFP instruction runs under the guest's FPCR, which the
// executor installs in the host before the first block (the native floating
// point environment), and an Advanced SIMD floating-point instruction under
// the Standard FPSCR value: FZ and DN set, round to nearest, AHP and FZ16 the
// guest's. The host FPCR keeps the mode it has across exits and links, as the
// gadget engine's does: CpuState::a32_fp_standard records it, a translation
// switches only where the instruction needs the other mode, and tests the
// byte at the first such instruction (set_fp_mode). fp_mode_ is what the
// translation knows of the host at the point being translated.

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_operations.hpp"
#include "a32_vector_encoders.hpp"

namespace uml::cpu::arm64::jit::a32 {

namespace {

constexpr std::uint32_t fpcr_standard_bits = 0x0300'0000U; // DN and FZ
constexpr std::uint32_t fpcr_guest_kept_bits = 0x0408'0000U; // AHP and FZ16

} // namespace

// ---------------------------------------------------------------------------
// The register views.

unsigned A32Compiler::q_use(unsigned q, unsigned fallback) {
  return read_native_vector(q, fallback).host;
}

JitCompilerFamilies::NativeVector A32Compiler::q_def(unsigned q,
                                                     bool reads_old,
                                                     unsigned fallback) {
  return write_native_vector(q, reads_old, fallback);
}

void A32Compiler::q_done(NativeVector value, unsigned q) {
  commit_native_vector(value, q);
}

unsigned A32Compiler::d_use(unsigned d, unsigned scratch) {
  const unsigned host = read_native_vector(d >> 1, scratch).host;
  if ((d & 1U) == 0U)
    return host;
  // DUP Dscratch, Vhost.D[1] (the scalar form clears the upper half).
  emit(enc::vec::dup_scalar(3, scratch, host, 1));
  return scratch;
}

unsigned A32Compiler::d_use_twice(unsigned d, unsigned scratch) {
  const unsigned host = read_native_vector(d >> 1, scratch).host;
  emit(enc::vec::dup_element(true, 3, scratch, host, d & 1U));
  return scratch;
}

void A32Compiler::d_write(unsigned d, unsigned result) {
  const auto destination = q_def(d >> 1, true, 2);
  emit(enc::vec::ins_element(3, destination.host, d & 1U, result, 0));
  q_done(destination, d >> 1);
}

int A32Compiler::s_shadow_host(unsigned s) noexcept {
  if (!s_shadowing_)
    return -1;
  for (std::size_t slot = 0; slot < s_shadows_.size(); ++slot) {
    if (s_shadows_[slot].s != static_cast<int>(s))
      continue;
    s_shadows_[slot].used = ++s_shadow_clock_;
    s_shadow_pinned_ |= 1U << slot;
    return static_cast<int>(s_shadow_hosts[slot]);
  }
  return -1;
}

unsigned A32Compiler::s_use(unsigned s, unsigned scratch) {
  if (const int shadow = s_shadow_host(s); shadow >= 0)
    return static_cast<unsigned>(shadow);
  const unsigned host = read_native_vector(s >> 2, scratch).host;
  if ((s & 3U) == 0U)
    return host;
  const unsigned into = s_shadowing_ ? take_s_shadow(s) : scratch;
  emit(enc::vec::dup_scalar(2, into, host, s & 3U));
  return into;
}

void A32Compiler::s_write(unsigned s, unsigned result) {
  bool shadowed = false;
  for (std::size_t slot = 0; slot < s_shadows_.size(); ++slot)
    if (s_shadows_[slot].s == static_cast<int>(s))
      shadowed = s_shadowing_ && s_shadow_hosts[slot] == result;
  if (!shadowed)
    forget_s_shadow(s);
  /* The fallback of a Q register that has no host is not a shadow's while
   * the instruction may use them: V6, which it has read by now. */
  const auto destination = q_def(s >> 2, true, s_shadowing_ ? 6U : 2U);
  emit(enc::vec::ins_element(2, destination.host, s & 3U, result, 0));
  q_done(destination, s >> 2);
}

// ---------------------------------------------------------------------------
// S shadows.

namespace {

enum class ShadowEffect : std::uint8_t {
  /* Leaves the shadows' registers and the S registers alone. A TLB miss
   * preserves them (the bridge saves V0-V7); a core register access across
   * a page takes the library, and the staged copy through V0-V3
   * (emit_crossing_cold) serves only the vector loads, which forget them. */
  keep,
  /* VLDR of one S register, whose value the shadow takes. */
  load,
  /* An unconditional single-precision VFP operation, which may use them. */
  use,
  /* Anything else. */
  forget,
};

ShadowEffect shadow_effect(const aarch32::Instruction &inst) noexcept {
  if (inst.status == aarch32::Status::nop)
    return ShadowEffect::keep;
  if (inst.status != aarch32::Status::ok)
    return ShadowEffect::forget;
  switch (op_info(inst.form).op) {
  case Op::adc: case Op::add: case Op::addw: case Op::adr: case Op::and_:
  case Op::bic: case Op::cmn: case Op::cmp: case Op::eor: case Op::mov:
  case Op::mvn: case Op::orn: case Op::orr: case Op::rsb: case Op::rsc:
  case Op::sbc: case Op::sub: case Op::subw: case Op::teq: case Op::tst:
  case Op::movw: case Op::movt:
  case Op::bfc: case Op::bfi: case Op::sbfx: case Op::ubfx: case Op::clz:
  case Op::rbit: case Op::rev: case Op::rev16: case Op::revsh: case Op::sxtb:
  case Op::sxth: case Op::uxtb: case Op::uxth: case Op::sxtab: case Op::sxtah:
  case Op::uxtab: case Op::uxtah: case Op::sxtb16: case Op::uxtb16:
  case Op::sxtab16: case Op::uxtab16: case Op::pkh: case Op::sel:
  case Op::mul: case Op::mla: case Op::mls: case Op::umull: case Op::umlal:
  case Op::smull: case Op::smlal: case Op::umaal: case Op::sdiv:
  case Op::udiv: case Op::smla_xy: case Op::smul_xy: case Op::smlal_xy:
  case Op::smlaw: case Op::smulw: case Op::smlad: case Op::smlsd:
  case Op::smuad: case Op::smusd: case Op::smlald: case Op::smlsld:
  case Op::smmla: case Op::smmls: case Op::smmul:
  case Op::qadd: case Op::qsub: case Op::qdadd: case Op::qdsub:
  case Op::ssat: case Op::usat:
  case Op::b: case Op::bl: case Op::blx: case Op::bx: case Op::cbz:
  case Op::cbnz: case Op::svc: case Op::bkpt: case Op::mrs: case Op::msr:
  case Op::nop: case Op::dmb: case Op::dsb: case Op::isb: case Op::clrex:
  case Op::it:
    return ShadowEffect::keep;
  case Op::ldr: case Op::ldrb: case Op::ldrh: case Op::ldrsb: case Op::ldrsh:
  case Op::ldrd: case Op::ldm: case Op::str: case Op::strb: case Op::strh:
  case Op::strd: case Op::stm: case Op::ldrt: case Op::ldrbt: case Op::ldrht:
  case Op::ldrsbt: case Op::ldrsht: case Op::strt: case Op::strbt:
  case Op::strht: case Op::lda: case Op::ldab: case Op::ldah: case Op::stl:
  case Op::stlb: case Op::stlh: case Op::tb: case Op::prefetch:
    return ShadowEffect::keep;
  case Op::vector:
    break;
  default:
    return ShadowEffect::forget;
  }
  const auto &v = vector_info(inst.form);
  switch (v.mnemonic) {
  case VM::vmrs: case VM::vmsr: case VM::vstr:
    return ShadowEffect::keep;
  default:
    break;
  }
  if (inst.cond < 14U || !v.vfp || v.dp)
    return ShadowEffect::forget;
  switch (v.mnemonic) {
  case VM::vldr:
    return ShadowEffect::load;
  case VM::vabs: case VM::vneg: case VM::vsqrt: case VM::vadd: case VM::vsub:
  case VM::vmul: case VM::vdiv: case VM::vnmul: case VM::vmla: case VM::vmls:
  case VM::vnmla: case VM::vnmls: case VM::vfma: case VM::vfms:
  case VM::vfnma: case VM::vfnms: case VM::vcmp: case VM::vcmpe:
    return ShadowEffect::use;
  case VM::vcvt: case VM::vcvtr:
    return v.variant == Variant::siv || v.variant == Variant::uiv ||
                   v.variant == Variant::vi
               ? ShadowEffect::use
               : ShadowEffect::forget;
  case VM::vmov:
    return v.variant == Variant::tos || v.variant == Variant::s ||
                   v.variant == Variant::r || v.variant == Variant::i
               ? ShadowEffect::use
               : ShadowEffect::forget;
  default:
    return ShadowEffect::forget;
  }
}

} // namespace

void A32Compiler::begin_s_shadows() {
  s_shadow_pinned_ = 0;
  s_shadowing_ = false;
  /* Without vector residency a Q register is staged in a fallback, V2
   * among them. */
  const bool enabled = !track_direct_stores_;
  const auto &inst = cur().inst;
  switch (shadow_effect(inst)) {
  case ShadowEffect::keep:
    break;
  case ShadowEffect::load:
    forget_s_shadow(static_cast<unsigned>(inst.operands.d));
    s_shadowing_ = enabled;
    break;
  case ShadowEffect::use:
    s_shadowing_ = enabled;
    break;
  case ShadowEffect::forget:
    forget_s_shadows();
    break;
  }
}

void A32Compiler::forget_s_shadows() noexcept {
  for (auto &shadow : s_shadows_)
    shadow.s = -1;
}

void A32Compiler::forget_s_shadow(unsigned s) noexcept {
  for (auto &shadow : s_shadows_)
    if (shadow.s == static_cast<int>(s))
      shadow.s = -1;
}

unsigned A32Compiler::take_s_shadow(unsigned s) noexcept {
  std::size_t chosen = s_shadows_.size();
  for (std::size_t slot = 0; slot < s_shadows_.size(); ++slot)
    if (s_shadows_[slot].s == static_cast<int>(s))
      chosen = slot;
  if (chosen == s_shadows_.size()) {
    for (std::size_t slot = 0; slot < s_shadows_.size(); ++slot) {
      if ((s_shadow_pinned_ & (1U << slot)) != 0U)
        continue;
      if (chosen == s_shadows_.size() || s_shadows_[slot].s < 0 ||
          (s_shadows_[chosen].s >= 0 &&
           s_shadows_[slot].used < s_shadows_[chosen].used))
        chosen = slot;
      if (s_shadows_[chosen].s < 0)
        break;
    }
  }
  s_shadows_[chosen] = {.s = static_cast<int>(s), .used = ++s_shadow_clock_};
  s_shadow_pinned_ |= 1U << chosen;
  return s_shadow_hosts[chosen];
}

// ---------------------------------------------------------------------------
// The floating-point environment.

void A32Compiler::touch_fp_environment() {
  touch_vectors();
  mark_native_fp();
}

/* The Standard FPSCR value is the guest's AHP and FZ16 with FZ and DN set.
 * CpuState::a32_fp_standard says which one the host FPCR holds while the
 * native environment is active, as it does for the gadget engine, and the
 * executor clears it where it activates the environment. Where the
 * translation does not know the mode it tests the byte and writes the FPCR
 * only if the mode differs; an exit leaves the FPCR as it is. */
/* The host FPCR takes the Standard value or the guest's, without a record. */
void A32Compiler::emit_fp_write(bool standard) {
  emitter_.load_w(0, 15, offsetof(CpuState, fpcr));
  if (standard) {
    emit(enc::movz(1, fpcr_guest_kept_bits >> 16, 1));
    emit(enc::and_(0, 0, 1));
    emit(enc::orr_i(0, 0, *enc::logical_immediate_field(fpcr_standard_bits)));
  }
  emit(0xd51b'4400U); // msr fpcr, x0
}

void A32Compiler::set_fp_mode(bool standard) {
  const auto wanted = standard ? FpMode::standard : FpMode::guest;
  if (!standard)
    ++guest_fp_mode_ops_;
  if (fp_mode_ == wanted)
    return;
  Arm64Emitter::Label done{};
  const bool tested = fp_mode_ == FpMode::unknown;
  if (tested) {
    done = make_label();
    emitter_.load_b(0, 15, offsetof(CpuState, a32_fp_standard));
    emitter_.compare_zero(done, 0, false, standard);
  }
  emit_fp_write(standard);
  if (standard) {
    emitter_.load_constant(1, 1);
    emitter_.store_b(1, 15, offsetof(CpuState, a32_fp_standard));
  } else {
    emitter_.store_b(31, 15, offsetof(CpuState, a32_fp_standard));
  }
  if (tested)
    bind(done);
  fp_mode_ = wanted;
}

/* The word of a Neon data-processing or structure instruction in the A32
 * layout (a Thumb word has its U bit apart from the rest). */
std::uint32_t A32Compiler::neon_word() const noexcept {
  const auto word = cur().inst.word;
  if (!thumb_)
    return word;
  if ((word & 0xff00'0000U) == 0xf900'0000U)
    return 0xf400'0000U | (word & 0x00ff'ffffU);
  return 0xf200'0000U | (((word >> 28) & 1U) << 24) | (word & 0x00ff'ffffU);
}

// ---------------------------------------------------------------------------
// The dispatch.

A32Compiler::Flow A32Compiler::emit_vector(const OpInfo &info) {
  touch_vectors();
  const auto &v = vector_info(cur().inst.form);
  if (v.mnemonic == VM::none) {
    exit_illegal();
    return Flow::end;
  }
  if (vector_memory(v.mnemonic))
    return emit_vector_memory(v);
  if (v.vfp)
    return emit_vfp(info, v);
  return emit_neon(v);
}

} // namespace uml::cpu::arm64::jit::a32
