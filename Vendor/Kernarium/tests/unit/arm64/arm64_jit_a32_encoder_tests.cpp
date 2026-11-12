// SPDX-License-Identifier: GPL-2.0-or-later
/* The A64 encoders of the AArch32 compiler (jit/a32/a32_encoders.hpp)
 * against the words LLVM MC assembles for the same instructions, including
 * every logical-immediate shape the lowering meets, and the logical
 * immediate recogniser against values that have none. */
#include "cpu/arm64/jit/a32/a32_encoders.hpp"
#include "require.hpp"

#include <cstdint>
#include <cstdio>

namespace {

using ::uml::testing::require;
namespace enc = ::uml::cpu::arm64::jit::a32::enc;

unsigned mismatches = 0;

void check(const char *assembly, std::uint32_t encoded, std::uint32_t expected) {
  if (encoded == expected)
    return;
  std::fprintf(stderr, "%s: encoder gave %08x, LLVM MC gives %08x\n", assembly,
               encoded, expected);
  ++mismatches;
}

/* The rows are generated from llvm-mc (aarch64, +v8.4a +flagm): the assembly
 * text, the encoder call and the word MC assembles it to. */
void encoders_match_llvm_mc() {
    check("add w3, w4, w5", enc::add(3, 4, 5), 0x0b050083U);
    check("add w9, w10, w11, lsr #7", enc::add(9, 10, 11, enc::lsr, 7), 0x0b4b1d49U);
    check("add w0, w30, w17, asr #31", enc::add(0, 30, 17, enc::asr, 31), 0x0b917fc0U);
    check("adds w3, w4, w5", enc::adds(3, 4, 5), 0x2b050083U);
    check("adds w9, w10, w11, lsr #7", enc::adds(9, 10, 11, enc::lsr, 7), 0x2b4b1d49U);
    check("adds w0, w30, w17, asr #31", enc::adds(0, 30, 17, enc::asr, 31), 0x2b917fc0U);
    check("sub w3, w4, w5", enc::sub(3, 4, 5), 0x4b050083U);
    check("sub w9, w10, w11, lsr #7", enc::sub(9, 10, 11, enc::lsr, 7), 0x4b4b1d49U);
    check("sub w0, w30, w17, asr #31", enc::sub(0, 30, 17, enc::asr, 31), 0x4b917fc0U);
    check("subs w3, w4, w5", enc::subs(3, 4, 5), 0x6b050083U);
    check("subs w9, w10, w11, lsr #7", enc::subs(9, 10, 11, enc::lsr, 7), 0x6b4b1d49U);
    check("subs w0, w30, w17, asr #31", enc::subs(0, 30, 17, enc::asr, 31), 0x6b917fc0U);
    check("and w3, w4, w5", enc::and_(3, 4, 5), 0x0a050083U);
    check("and w9, w10, w11, lsr #7", enc::and_(9, 10, 11, enc::lsr, 7), 0x0a4b1d49U);
    check("and w0, w30, w17, asr #31", enc::and_(0, 30, 17, enc::asr, 31), 0x0a917fc0U);
    check("ands w3, w4, w5", enc::ands(3, 4, 5), 0x6a050083U);
    check("ands w9, w10, w11, lsr #7", enc::ands(9, 10, 11, enc::lsr, 7), 0x6a4b1d49U);
    check("ands w0, w30, w17, asr #31", enc::ands(0, 30, 17, enc::asr, 31), 0x6a917fc0U);
    check("bic w3, w4, w5", enc::bic(3, 4, 5), 0x0a250083U);
    check("bic w9, w10, w11, lsr #7", enc::bic(9, 10, 11, enc::lsr, 7), 0x0a6b1d49U);
    check("bic w0, w30, w17, asr #31", enc::bic(0, 30, 17, enc::asr, 31), 0x0ab17fc0U);
    check("bics w3, w4, w5", enc::bics(3, 4, 5), 0x6a250083U);
    check("bics w9, w10, w11, lsr #7", enc::bics(9, 10, 11, enc::lsr, 7), 0x6a6b1d49U);
    check("bics w0, w30, w17, asr #31", enc::bics(0, 30, 17, enc::asr, 31), 0x6ab17fc0U);
    check("orr w3, w4, w5", enc::orr(3, 4, 5), 0x2a050083U);
    check("orr w9, w10, w11, lsr #7", enc::orr(9, 10, 11, enc::lsr, 7), 0x2a4b1d49U);
    check("orr w0, w30, w17, asr #31", enc::orr(0, 30, 17, enc::asr, 31), 0x2a917fc0U);
    check("orn w3, w4, w5", enc::orn(3, 4, 5), 0x2a250083U);
    check("orn w9, w10, w11, lsr #7", enc::orn(9, 10, 11, enc::lsr, 7), 0x2a6b1d49U);
    check("orn w0, w30, w17, asr #31", enc::orn(0, 30, 17, enc::asr, 31), 0x2ab17fc0U);
    check("eor w3, w4, w5", enc::eor(3, 4, 5), 0x4a050083U);
    check("eor w9, w10, w11, lsr #7", enc::eor(9, 10, 11, enc::lsr, 7), 0x4a4b1d49U);
    check("eor w0, w30, w17, asr #31", enc::eor(0, 30, 17, enc::asr, 31), 0x4a917fc0U);
    check("orr x3, x4, x5", enc::x::orr(3, 4, 5), 0xaa050083U);
    check("orr x20, x21, x22, lsl #12", enc::x::orr(20, 21, 22, enc::lsl, 12), 0xaa1632b4U);
    check("and x3, x4, x5", enc::x::and_(3, 4, 5), 0x8a050083U);
    check("and x20, x21, x22, lsl #12", enc::x::and_(20, 21, 22, enc::lsl, 12), 0x8a1632b4U);
    check("bic x3, x4, x5", enc::x::bic(3, 4, 5), 0x8a250083U);
    check("bic x20, x21, x22, lsl #12", enc::x::bic(20, 21, 22, enc::lsl, 12), 0x8a3632b4U);
    check("add x3, x4, x5", enc::x::add(3, 4, 5), 0x8b050083U);
    check("add x20, x21, x22, lsl #12", enc::x::add(20, 21, 22, enc::lsl, 12), 0x8b1632b4U);
    check("sub x3, x4, x5", enc::x::sub(3, 4, 5), 0xcb050083U);
    check("sub x20, x21, x22, lsl #12", enc::x::sub(20, 21, 22, enc::lsl, 12), 0xcb1632b4U);
    check("mov w3, w4", enc::mov(3, 4), 0x2a0403e3U);
    check("mov x3, x4", enc::x::mov(3, 4), 0xaa0403e3U);
    check("mvn w3, w4", enc::mvn(3, 4), 0x2a2403e3U);
    check("mvn w3, w4, lsl #5", enc::mvn(3, 4, enc::lsl, 5), 0x2a2417e3U);
    check("cmp w3, w4", enc::cmp(3, 4), 0x6b04007fU);
    check("cmp w3, w4, asr #9", enc::cmp(3, 4, enc::asr, 9), 0x6b84247fU);
    check("cmn w3, w4, lsl #2", enc::cmn(3, 4, enc::lsl, 2), 0x2b04087fU);
    check("tst w3, w4, lsr #1", enc::tst(3, 4, enc::lsr, 1), 0x6a44047fU);
    check("tst x3, x4", enc::x::tst(3, 4), 0xea04007fU);
    check("cmp x3, x4", enc::x::cmp(3, 4), 0xeb04007fU);
    check("add x1, x2, w3, sxtw", enc::x::add_sxtw(1, 2, 3), 0x8b23c041U);
    check("add x1, x2, w3, sxtw #2", enc::x::add_sxtw(1, 2, 3, 2), 0x8b23c841U);
    check("adc w1, w2, w3", enc::adc(1, 2, 3), 0x1a030041U);
    check("adcs w1, w2, w3", enc::adcs(1, 2, 3), 0x3a030041U);
    check("sbc w1, w2, w3", enc::sbc(1, 2, 3), 0x5a030041U);
    check("sbcs w1, w2, w3", enc::sbcs(1, 2, 3), 0x7a030041U);
    check("add w3, w4, #7", enc::add_i(3, 4, 7), 0x11001c83U);
    check("add w3, w4, #4095", enc::add_i(3, 4, 4095), 0x113ffc83U);
    check("add w3, w4, #5, lsl #12", enc::add_i(3, 4, 5, true), 0x11401483U);
    check("adds w3, w4, #7", enc::adds_i(3, 4, 7), 0x31001c83U);
    check("adds w3, w4, #4095", enc::adds_i(3, 4, 4095), 0x313ffc83U);
    check("adds w3, w4, #5, lsl #12", enc::adds_i(3, 4, 5, true), 0x31401483U);
    check("sub w3, w4, #7", enc::sub_i(3, 4, 7), 0x51001c83U);
    check("sub w3, w4, #4095", enc::sub_i(3, 4, 4095), 0x513ffc83U);
    check("sub w3, w4, #5, lsl #12", enc::sub_i(3, 4, 5, true), 0x51401483U);
    check("subs w3, w4, #7", enc::subs_i(3, 4, 7), 0x71001c83U);
    check("subs w3, w4, #4095", enc::subs_i(3, 4, 4095), 0x713ffc83U);
    check("subs w3, w4, #5, lsl #12", enc::subs_i(3, 4, 5, true), 0x71401483U);
    check("cmp w3, #200", enc::cmp_i(3, 200), 0x7103207fU);
    check("cmp x3, #200", enc::x::cmp_i(3, 200), 0xf103207fU);
    check("and w3, w4, #0xfffffffe", enc::and_i(3, 4, *enc::logical_immediate_field(0xfffffffeU)), 0x121f7883U);
    check("orr w3, w4, #0xfffffffe", enc::orr_i(3, 4, *enc::logical_immediate_field(0xfffffffeU)), 0x321f7883U);
    check("and w3, w4, #0xff", enc::and_i(3, 4, *enc::logical_immediate_field(0xffU)), 0x12001c83U);
    check("orr w3, w4, #0xff", enc::orr_i(3, 4, *enc::logical_immediate_field(0xffU)), 0x32001c83U);
    check("and w3, w4, #0x80808080", enc::and_i(3, 4, *enc::logical_immediate_field(0x80808080U)), 0x1201c083U);
    check("orr w3, w4, #0x80808080", enc::orr_i(3, 4, *enc::logical_immediate_field(0x80808080U)), 0x3201c083U);
    check("and w3, w4, #0xf0000", enc::and_i(3, 4, *enc::logical_immediate_field(0xf0000U)), 0x12100c83U);
    check("orr w3, w4, #0xf0000", enc::orr_i(3, 4, *enc::logical_immediate_field(0xf0000U)), 0x32100c83U);
    check("and w3, w4, #0x1010101", enc::and_i(3, 4, *enc::logical_immediate_field(0x1010101U)), 0x1200c083U);
    check("orr w3, w4, #0x1010101", enc::orr_i(3, 4, *enc::logical_immediate_field(0x1010101U)), 0x3200c083U);
    check("and w3, w4, #0x7fff7fff", enc::and_i(3, 4, *enc::logical_immediate_field(0x7fff7fffU)), 0x1200b883U);
    check("orr w3, w4, #0x7fff7fff", enc::orr_i(3, 4, *enc::logical_immediate_field(0x7fff7fffU)), 0x3200b883U);
    check("and w3, w4, #0x80008000", enc::and_i(3, 4, *enc::logical_immediate_field(0x80008000U)), 0x12018083U);
    check("orr w3, w4, #0x80008000", enc::orr_i(3, 4, *enc::logical_immediate_field(0x80008000U)), 0x32018083U);
    check("and w3, w4, #0xff00ff", enc::and_i(3, 4, *enc::logical_immediate_field(0xff00ffU)), 0x12009c83U);
    check("orr w3, w4, #0xff00ff", enc::orr_i(3, 4, *enc::logical_immediate_field(0xff00ffU)), 0x32009c83U);
    check("and w3, w4, #0x1", enc::and_i(3, 4, *enc::logical_immediate_field(0x1U)), 0x12000083U);
    check("orr w3, w4, #0x1", enc::orr_i(3, 4, *enc::logical_immediate_field(0x1U)), 0x32000083U);
    check("and w3, w4, #0x80000000", enc::and_i(3, 4, *enc::logical_immediate_field(0x80000000U)), 0x12010083U);
    check("orr w3, w4, #0x80000000", enc::orr_i(3, 4, *enc::logical_immediate_field(0x80000000U)), 0x32010083U);
    check("and w3, w4, #0xaaaaaaaa", enc::and_i(3, 4, *enc::logical_immediate_field(0xaaaaaaaaU)), 0x1201f083U);
    check("orr w3, w4, #0xaaaaaaaa", enc::orr_i(3, 4, *enc::logical_immediate_field(0xaaaaaaaaU)), 0x3201f083U);
    check("and w3, w4, #0x55555555", enc::and_i(3, 4, *enc::logical_immediate_field(0x55555555U)), 0x1200f083U);
    check("orr w3, w4, #0x55555555", enc::orr_i(3, 4, *enc::logical_immediate_field(0x55555555U)), 0x3200f083U);
    check("and w3, w4, #0xfffffff0", enc::and_i(3, 4, *enc::logical_immediate_field(0xfffffff0U)), 0x121c6c83U);
    check("orr w3, w4, #0xfffffff0", enc::orr_i(3, 4, *enc::logical_immediate_field(0xfffffff0U)), 0x321c6c83U);
    check("and w3, w4, #0xff00ff0", enc::and_i(3, 4, *enc::logical_immediate_field(0xff00ff0U)), 0x120c9c83U);
    check("orr w3, w4, #0xff00ff0", enc::orr_i(3, 4, *enc::logical_immediate_field(0xff00ff0U)), 0x320c9c83U);
    check("and w3, w4, #0x3", enc::and_i(3, 4, *enc::logical_immediate_field(0x3U)), 0x12000483U);
    check("orr w3, w4, #0x3", enc::orr_i(3, 4, *enc::logical_immediate_field(0x3U)), 0x32000483U);
    check("and w3, w4, #0x7fffffff", enc::and_i(3, 4, *enc::logical_immediate_field(0x7fffffffU)), 0x12007883U);
    check("orr w3, w4, #0x7fffffff", enc::orr_i(3, 4, *enc::logical_immediate_field(0x7fffffffU)), 0x32007883U);
    check("and w3, w4, #0xc0000003", enc::and_i(3, 4, *enc::logical_immediate_field(0xc0000003U)), 0x12020c83U);
    check("orr w3, w4, #0xc0000003", enc::orr_i(3, 4, *enc::logical_immediate_field(0xc0000003U)), 0x32020c83U);
    check("and w3, w4, #0xf000000f", enc::and_i(3, 4, *enc::logical_immediate_field(0xf000000fU)), 0x12041c83U);
    check("orr w3, w4, #0xf000000f", enc::orr_i(3, 4, *enc::logical_immediate_field(0xf000000fU)), 0x32041c83U);
    check("eor w3, w4, #0xff00ff00", enc::eor_i(3, 4, *enc::logical_immediate_field(0xff00ff00U)), 0x52089c83U);
    check("ands w3, w4, #0xfffffffe", enc::ands_i(3, 4, *enc::logical_immediate_field(0xfffffffeU)), 0x721f7883U);
    check("movz w3, #0x1234", enc::movz(3, 0x1234), 0x52824683U);
    check("movz w3, #0x1234, lsl #16", enc::movz(3, 0x1234, 1), 0x52a24683U);
    check("movn w3, #0x1234", enc::movn(3, 0x1234), 0x12824683U);
    check("movk w3, #0xabcd, lsl #16", enc::movk(3, 0xabcd, 1), 0x72b579a3U);
    check("movz x3, #0x1234, lsl #32", enc::x::movz(3, 0x1234, 2), 0xd2c24683U);
    check("sbfm w3, w4, #5, #9", enc::sbfm(3, 4, 5, 9), 0x13052483U);
    check("bfm w3, w4, #5, #9", enc::bfm(3, 4, 5, 9), 0x33052483U);
    check("ubfm w3, w4, #5, #9", enc::ubfm(3, 4, 5, 9), 0x53052483U);
    check("ubfm x3, x4, #5, #9", enc::x::ubfm(3, 4, 5, 9), 0xd3452483U);
    check("sbfm x3, x4, #5, #9", enc::x::sbfm(3, 4, 5, 9), 0x93452483U);
    check("lsl w3, w4, #1", enc::lsl_i(3, 4, 1), 0x531f7883U);
    check("lsr w3, w4, #1", enc::lsr_i(3, 4, 1), 0x53017c83U);
    check("asr w3, w4, #1", enc::asr_i(3, 4, 1), 0x13017c83U);
    check("ror w3, w4, #1", enc::ror_i(3, 4, 1), 0x13840483U);
    check("lsl w3, w4, #7", enc::lsl_i(3, 4, 7), 0x53196083U);
    check("lsr w3, w4, #7", enc::lsr_i(3, 4, 7), 0x53077c83U);
    check("asr w3, w4, #7", enc::asr_i(3, 4, 7), 0x13077c83U);
    check("ror w3, w4, #7", enc::ror_i(3, 4, 7), 0x13841c83U);
    check("lsl w3, w4, #31", enc::lsl_i(3, 4, 31), 0x53010083U);
    check("lsr w3, w4, #31", enc::lsr_i(3, 4, 31), 0x531f7c83U);
    check("asr w3, w4, #31", enc::asr_i(3, 4, 31), 0x131f7c83U);
    check("ror w3, w4, #31", enc::ror_i(3, 4, 31), 0x13847c83U);
    check("lsl x3, x4, #1", enc::x::lsl_i(3, 4, 1), 0xd37ff883U);
    check("lsr x3, x4, #1", enc::x::lsr_i(3, 4, 1), 0xd341fc83U);
    check("asr x3, x4, #1", enc::x::asr_i(3, 4, 1), 0x9341fc83U);
    check("lsl x3, x4, #7", enc::x::lsl_i(3, 4, 7), 0xd379e083U);
    check("lsr x3, x4, #7", enc::x::lsr_i(3, 4, 7), 0xd347fc83U);
    check("asr x3, x4, #7", enc::x::asr_i(3, 4, 7), 0x9347fc83U);
    check("lsl x3, x4, #63", enc::x::lsl_i(3, 4, 63), 0xd3410083U);
    check("lsr x3, x4, #63", enc::x::lsr_i(3, 4, 63), 0xd37ffc83U);
    check("asr x3, x4, #63", enc::x::asr_i(3, 4, 63), 0x937ffc83U);
    check("extr w3, w4, w5, #13", enc::extr(3, 4, 5, 13), 0x13853483U);
    check("uxtb w3, w4", enc::uxtb(3, 4), 0x53001c83U);
    check("uxth w3, w4", enc::uxth(3, 4), 0x53003c83U);
    check("sxtb w3, w4", enc::sxtb(3, 4), 0x13001c83U);
    check("sxth w3, w4", enc::sxth(3, 4), 0x13003c83U);
    check("sxtw x3, w4", enc::x::sxtw(3, 4), 0x93407c83U);
    check("ubfx w3, w4, #5, #9", enc::ubfx(3, 4, 5, 9), 0x53053483U);
    check("sbfx w3, w4, #5, #9", enc::sbfx(3, 4, 5, 9), 0x13053483U);
    check("bfi w3, w4, #5, #9", enc::bfi(3, 4, 5, 9), 0x331b2083U);
    check("bfxil w3, w4, #5, #9", enc::bfxil(3, 4, 5, 9), 0x33053483U);
    check("ubfx x3, x4, #5, #9", enc::x::ubfx(3, 4, 5, 9), 0xd3453483U);
    check("lslv w3, w4, w5", enc::lslv(3, 4, 5), 0x1ac52083U);
    check("lsrv w3, w4, w5", enc::lsrv(3, 4, 5), 0x1ac52483U);
    check("asrv w3, w4, w5", enc::asrv(3, 4, 5), 0x1ac52883U);
    check("rorv w3, w4, w5", enc::rorv(3, 4, 5), 0x1ac52c83U);
    check("lslv x3, x4, x5", enc::x::lslv(3, 4, 5), 0x9ac52083U);
    check("lsrv x3, x4, x5", enc::x::lsrv(3, 4, 5), 0x9ac52483U);
    check("asrv x3, x4, x5", enc::x::asrv(3, 4, 5), 0x9ac52883U);
    check("madd w3, w4, w5, w6", enc::madd(3, 4, 5, 6), 0x1b051883U);
    check("msub w3, w4, w5, w6", enc::msub(3, 4, 5, 6), 0x1b059883U);
    check("smaddl x3, w4, w5, x6", enc::smaddl(3, 4, 5, 6), 0x9b251883U);
    check("umaddl x3, w4, w5, x6", enc::umaddl(3, 4, 5, 6), 0x9ba51883U);
    check("smsubl x3, w4, w5, x6", enc::smsubl(3, 4, 5, 6), 0x9b259883U);
    check("mul w3, w4, w5", enc::mul(3, 4, 5), 0x1b057c83U);
    check("smull x3, w4, w5", enc::smull(3, 4, 5), 0x9b257c83U);
    check("umull x3, w4, w5", enc::umull(3, 4, 5), 0x9ba57c83U);
    check("umull x3, w4, w5", enc::x::umull(3, 4, 5), 0x9ba57c83U);
    check("smull x3, w4, w5", enc::x::smull(3, 4, 5), 0x9b257c83U);
    check("mul x3, x4, x5", enc::x::mul(3, 4, 5), 0x9b057c83U);
    check("madd x3, x4, x5, x6", enc::x::madd(3, 4, 5, 6), 0x9b051883U);
    check("msub x3, x4, x5, x6", enc::x::msub(3, 4, 5, 6), 0x9b059883U);
    check("udiv w3, w4, w5", enc::udiv(3, 4, 5), 0x1ac50883U);
    check("sdiv w3, w4, w5", enc::sdiv(3, 4, 5), 0x1ac50c83U);
    check("crc32b w3, w4, w5", enc::crc32(0, false, 3, 4, 5), 0x1ac54083U);
    check("crc32h w3, w4, w5", enc::crc32(1, false, 3, 4, 5), 0x1ac54483U);
    check("crc32w w3, w4, w5", enc::crc32(2, false, 3, 4, 5), 0x1ac54883U);
    check("crc32cb w3, w4, w5", enc::crc32(0, true, 3, 4, 5), 0x1ac55083U);
    check("crc32ch w3, w4, w5", enc::crc32(1, true, 3, 4, 5), 0x1ac55483U);
    check("crc32cw w3, w4, w5", enc::crc32(2, true, 3, 4, 5), 0x1ac55883U);
    check("clz w3, w4", enc::clz(3, 4), 0x5ac01083U);
    check("rbit w3, w4", enc::rbit(3, 4), 0x5ac00083U);
    check("rev w3, w4", enc::rev(3, 4), 0x5ac00883U);
    check("rev16 w3, w4", enc::rev16(3, 4), 0x5ac00483U);
    check("csel w3, w4, w5, eq", enc::csel(3, 4, 5, 0), 0x1a850083U);
    check("csinc w3, w4, w5, eq", enc::csinc(3, 4, 5, 0), 0x1a850483U);
    check("cset w3, eq", enc::cset(3, 0), 0x1a9f17e3U);
    check("csel w3, w4, w5, ne", enc::csel(3, 4, 5, 1), 0x1a851083U);
    check("csinc w3, w4, w5, ne", enc::csinc(3, 4, 5, 1), 0x1a851483U);
    check("cset w3, ne", enc::cset(3, 1), 0x1a9f07e3U);
    check("csel w3, w4, w5, ge", enc::csel(3, 4, 5, 10), 0x1a85a083U);
    check("csinc w3, w4, w5, ge", enc::csinc(3, 4, 5, 10), 0x1a85a483U);
    check("cset w3, ge", enc::cset(3, 10), 0x1a9fb7e3U);
    check("csel w3, w4, w5, le", enc::csel(3, 4, 5, 13), 0x1a85d083U);
    check("csinc w3, w4, w5, le", enc::csinc(3, 4, 5, 13), 0x1a85d483U);
    check("cset w3, le", enc::cset(3, 13), 0x1a9fc7e3U);
    check("msr nzcv, x3", enc::msr_nzcv(3), 0xd51b4203U);
    check("rmif x3, #5, #9", enc::rmif(3, 5, 9), 0xba028469U);
    check("ldr w3, [x4, #8]", enc::ldr_w(3, 4, 8), 0xb9400883U);
    check("str w3, [x4, #8]", enc::str_w(3, 4, 8), 0xb9000883U);
    check("ldr x3, [x4, #16]", enc::ldr_x(3, 4, 16), 0xf9400883U);
    check("str x3, [x4, #16]", enc::str_x(3, 4, 16), 0xf9000883U);
    check("ldr x3, [x4, #16]", enc::x::ldr(3, 4, 16), 0xf9400883U);
    check("str x3, [x4, #16]", enc::x::str(3, 4, 16), 0xf9000883U);
    check("ldrb w3, [x4, #5]", enc::ldrb(3, 4, 5), 0x39401483U);
    check("strb w3, [x4, #5]", enc::strb(3, 4, 5), 0x39001483U);
    check("ldrh w3, [x4, #6]", enc::ldrh(3, 4, 6), 0x79400c83U);
    check("strh w3, [x4, #6]", enc::strh(3, 4, 6), 0x79000c83U);
    check("ldrsb w3, [x4, #5]", enc::ldrsb(3, 4, 5), 0x39c01483U);
    check("ldrsh w3, [x4, #6]", enc::ldrsh(3, 4, 6), 0x79c00c83U);
    check("ldxrb w3, [x4]", enc::ldxr(0, 3, 4), 0x085f7c83U);
    check("ldaxrb w3, [x4]", enc::ldxr(0, 3, 4, true), 0x085ffc83U);
    check("stxrb w6, w3, [x4]", enc::stxr(0, 6, 3, 4), 0x08067c83U);
    check("stlxrb w6, w3, [x4]", enc::stxr(0, 6, 3, 4, true), 0x0806fc83U);
    check("ldxrh w3, [x4]", enc::ldxr(1, 3, 4), 0x485f7c83U);
    check("ldaxrh w3, [x4]", enc::ldxr(1, 3, 4, true), 0x485ffc83U);
    check("stxrh w6, w3, [x4]", enc::stxr(1, 6, 3, 4), 0x48067c83U);
    check("stlxrh w6, w3, [x4]", enc::stxr(1, 6, 3, 4, true), 0x4806fc83U);
    check("ldxr w3, [x4]", enc::ldxr(2, 3, 4), 0x885f7c83U);
    check("ldaxr w3, [x4]", enc::ldxr(2, 3, 4, true), 0x885ffc83U);
    check("stxr w6, w3, [x4]", enc::stxr(2, 6, 3, 4), 0x88067c83U);
    check("stlxr w6, w3, [x4]", enc::stxr(2, 6, 3, 4, true), 0x8806fc83U);
    check("ldxr x3, [x4]", enc::ldxr(3, 3, 4), 0xc85f7c83U);
    check("stxr w6, x3, [x4]", enc::stxr(3, 6, 3, 4), 0xc8067c83U);
    check("ldarb w3, [x4]", enc::ldar(0, 3, 4), 0x08dffc83U);
    check("stlrb w3, [x4]", enc::stlr(0, 3, 4), 0x089ffc83U);
    check("ldarh w3, [x4]", enc::ldar(1, 3, 4), 0x48dffc83U);
    check("stlrh w3, [x4]", enc::stlr(1, 3, 4), 0x489ffc83U);
    check("ldar w3, [x4]", enc::ldar(2, 3, 4), 0x88dffc83U);
    check("stlr w3, [x4]", enc::stlr(2, 3, 4), 0x889ffc83U);
    check("ldar x3, [x4]", enc::ldar(3, 3, 4), 0xc8dffc83U);
    check("stlr x3, [x4]", enc::stlr(3, 3, 4), 0xc89ffc83U);
    check("nop", enc::nop(), 0xd503201fU);
    check("dmb ish", enc::dmb_ish(), 0xd5033bbfU);
    check("dmb ishst", enc::dmb_ishst(), 0xd5033abfU);
    check("dsb sy", enc::dsb_sy(), 0xd5033f9fU);
    check("isb", enc::isb(), 0xd5033fdfU);
    check("add v3.8b, v4.8b, v5.8b", enc::v::add(0, 3, 4, 5), 0x0e258483U);
    check("sub v3.8b, v4.8b, v5.8b", enc::v::sub(0, 3, 4, 5), 0x2e258483U);
    check("sqadd v3.8b, v4.8b, v5.8b", enc::v::sqadd(0, 3, 4, 5), 0x0e250c83U);
    check("uqadd v3.8b, v4.8b, v5.8b", enc::v::uqadd(0, 3, 4, 5), 0x2e250c83U);
    check("sqsub v3.8b, v4.8b, v5.8b", enc::v::sqsub(0, 3, 4, 5), 0x0e252c83U);
    check("uqsub v3.8b, v4.8b, v5.8b", enc::v::uqsub(0, 3, 4, 5), 0x2e252c83U);
    check("shadd v3.8b, v4.8b, v5.8b", enc::v::shadd(0, 3, 4, 5), 0x0e250483U);
    check("uhadd v3.8b, v4.8b, v5.8b", enc::v::uhadd(0, 3, 4, 5), 0x2e250483U);
    check("shsub v3.8b, v4.8b, v5.8b", enc::v::shsub(0, 3, 4, 5), 0x0e252483U);
    check("uhsub v3.8b, v4.8b, v5.8b", enc::v::uhsub(0, 3, 4, 5), 0x2e252483U);
    check("add v3.4h, v4.4h, v5.4h", enc::v::add(1, 3, 4, 5), 0x0e658483U);
    check("sub v3.4h, v4.4h, v5.4h", enc::v::sub(1, 3, 4, 5), 0x2e658483U);
    check("sqadd v3.4h, v4.4h, v5.4h", enc::v::sqadd(1, 3, 4, 5), 0x0e650c83U);
    check("uqadd v3.4h, v4.4h, v5.4h", enc::v::uqadd(1, 3, 4, 5), 0x2e650c83U);
    check("sqsub v3.4h, v4.4h, v5.4h", enc::v::sqsub(1, 3, 4, 5), 0x0e652c83U);
    check("uqsub v3.4h, v4.4h, v5.4h", enc::v::uqsub(1, 3, 4, 5), 0x2e652c83U);
    check("shadd v3.4h, v4.4h, v5.4h", enc::v::shadd(1, 3, 4, 5), 0x0e650483U);
    check("uhadd v3.4h, v4.4h, v5.4h", enc::v::uhadd(1, 3, 4, 5), 0x2e650483U);
    check("shsub v3.4h, v4.4h, v5.4h", enc::v::shsub(1, 3, 4, 5), 0x0e652483U);
    check("uhsub v3.4h, v4.4h, v5.4h", enc::v::uhsub(1, 3, 4, 5), 0x2e652483U);
    check("add v3.2s, v4.2s, v5.2s", enc::v::add(2, 3, 4, 5), 0x0ea58483U);
    check("sub v3.2s, v4.2s, v5.2s", enc::v::sub(2, 3, 4, 5), 0x2ea58483U);
    check("sqadd v3.2s, v4.2s, v5.2s", enc::v::sqadd(2, 3, 4, 5), 0x0ea50c83U);
    check("uqadd v3.2s, v4.2s, v5.2s", enc::v::uqadd(2, 3, 4, 5), 0x2ea50c83U);
    check("sqsub v3.2s, v4.2s, v5.2s", enc::v::sqsub(2, 3, 4, 5), 0x0ea52c83U);
    check("uqsub v3.2s, v4.2s, v5.2s", enc::v::uqsub(2, 3, 4, 5), 0x2ea52c83U);
    check("shadd v3.2s, v4.2s, v5.2s", enc::v::shadd(2, 3, 4, 5), 0x0ea50483U);
    check("uhadd v3.2s, v4.2s, v5.2s", enc::v::uhadd(2, 3, 4, 5), 0x2ea50483U);
    check("shsub v3.2s, v4.2s, v5.2s", enc::v::shsub(2, 3, 4, 5), 0x0ea52483U);
    check("uhsub v3.2s, v4.2s, v5.2s", enc::v::uhsub(2, 3, 4, 5), 0x2ea52483U);
  require(mismatches == 0);
}

void logical_immediates_without_an_encoding() {
  for (const std::uint32_t value :
       {0U, 0xffff'ffffU, 0x1234'5678U, 0x0000'0101U, 0x00ff'00feU, 0xff00'ff01U})
    require(!enc::logical_immediate_field(value).has_value());
  /* Every rotated run of ones in an element of 2, 4, 8, 16 and 32 bits
   * repeated across the word has an encoding. */
  unsigned encodable = 0;
  for (unsigned size = 2; size <= 32; size *= 2)
    for (unsigned ones = 1; ones < size; ++ones)
      for (unsigned rotation = 0; rotation < size; ++rotation) {
        const std::uint32_t mask =
            size == 32 ? 0xffff'ffffU : (1U << size) - 1U;
        std::uint32_t element = (1U << ones) - 1U;
        element = ((element << rotation) | (element >> ((size - rotation) % size))) & mask;
        std::uint32_t value = 0;
        for (unsigned at = 0; at < 32; at += size)
          value |= element << at;
        require(enc::logical_immediate_field(value).has_value());
        ++encodable;
      }
  require(encodable != 0);
}

} // namespace

int main() {
  encoders_match_llvm_mc();
  logical_immediates_without_an_encoding();
  std::puts("arm64-jit-a32-encoder: ok");
  return 0;
}
