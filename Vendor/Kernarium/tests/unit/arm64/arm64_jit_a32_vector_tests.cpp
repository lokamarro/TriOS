// SPDX-License-Identifier: GPL-2.0-or-later
/* The JIT's VFP and Advanced SIMD behaviour that the instruction corpus does
 * not see. The floating-point environment: a VFP instruction runs under the
 * guest's FPCR and an Advanced SIMD one under the Standard FPSCR value, across
 * a conditional arm, a direct branch (a chained block), an exit and the
 * executor's own re-entry, which leave the host FPCR as it is. The
 * cumulative saturation flag. The vector memory forms: that they stay out of
 * the semantic library on an ordinary access, and take it, with its data and
 * its faults, on a page crossing, a misalignment and an unmapped page. The
 * words were assembled with LLVM MC at authoring time.
 *
 * VMAX, VMIN, VADD, VSUB, VMUL, VMLA and VMLS run on every special operand,
 * rounding mode and flush mode in the contexts that guard them and the one
 * that does not, and QC across runs. The flags of VCMP and VCMPE reach the
 * APSR through VMRS, adjacent or not, conditional or not. Every vector load
 * form crosses a page at every offset, with the two pages adjacent in the
 * host and not, and takes no library call once the TLB holds both; the
 * library leaves the guest's FP environment installed. */
#include "arm64_jit_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/aarch32/semantics_exit_counters.hpp"

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace uml::emulator::tests;
using namespace uml::emulator::tests::a32;
using ::uml::cpu::ExitReason;

namespace {

namespace aarch32 = ::uml::cpu::arm64::aarch32;

constexpr std::uint32_t arm_svc = 0xef00'0000U;
constexpr std::uint32_t arm_nop = 0xe320'f000U;
constexpr std::uint32_t fpscr_qc = 1U << 27;

/* The lane `lane` (32 bits) of Q register `q`. */
std::uint32_t lane32(const arm64::CpuState &cpu, unsigned q, unsigned lane) {
  std::uint32_t value;
  std::memcpy(&value, cpu.vectors[q].bytes.data() + 4U * lane, 4);
  return value;
}
void set_lane32(arm64::CpuState &cpu, unsigned q, unsigned lane,
                std::uint32_t value) {
  std::memcpy(cpu.vectors[q].bytes.data() + 4U * lane, &value, 4);
}
std::uint64_t half64(const arm64::CpuState &cpu, unsigned q, unsigned half) {
  std::uint64_t value;
  std::memcpy(&value, cpu.vectors[q].bytes.data() + 8U * half, 8);
  return value;
}
void set_half64(arm64::CpuState &cpu, unsigned q, unsigned half,
                std::uint64_t value) {
  std::memcpy(cpu.vectors[q].bytes.data() + 8U * half, &value, 8);
}

/* Whether the program ran without a trip to the semantic library. */
struct ExitCount final {
  ExitCount() {
    aarch32::reset_a32_library_exits();
    aarch32::count_a32_library_exits(true);
  }
  ~ExitCount() { aarch32::count_a32_library_exits(false); }
  [[nodiscard]] static std::size_t total() { return aarch32::a32_library_exit_total(); }
};

// ---------------------------------------------------------------------------
// The floating-point environment.

constexpr std::uint32_t one = 0x3f80'0000U;
/* 1.5 * 2^-24: 1 + this is 3/4 of a unit in the last place above 1, which
 * rounds up to nearest and down toward zero. */
constexpr std::uint32_t three_quarters_ulp = 0x33c0'0000U;
constexpr std::uint32_t one_ulp_up = 0x3f80'0001U;
constexpr std::uint32_t sentinel = 0xdead'beefU;
constexpr std::uint32_t round_toward_zero = 0x00c0'0000U;

void load_fp_program(Fixture &fixture) {
  fixture.code32(0, {
      0xeee1'1a10U, // vmsr fpscr, r1
      0xee30'0a81U, // vadd.f32 s0, s1, s2     (VFP: the guest's FPCR)
      0xf204'3d05U, // vadd.f32 d3, d4, d5     (Neon: Standard FPSCR)
      0x1e30'8a81U, // vaddne.f32 s16, s1, s2  (a conditional arm)
      0xf204'7d05U, // vadd.f32 d7, d4, d5
      0x0e30'aa81U, // vaddeq.f32 s20, s1, s2
      0xea00'0000U, // b +1 (a direct branch: the next block is chained)
      arm_nop,
      0xee30'ca81U, // vadd.f32 s24, s1, s2    (the first of a block)
      arm_svc});
}

arm64::CpuState fp_task(std::uint32_t rounding, bool zero) {
  auto cpu = Fixture::task(false);
  cpu.registers[1] = rounding;
  cpu.nzcv = zero ? flag_z : 0U;
  set_lane32(cpu, 0, 0, 0x1111'1111U);
  set_lane32(cpu, 0, 1, one);
  set_lane32(cpu, 0, 2, three_quarters_ulp);
  set_lane32(cpu, 0, 3, 0x2222'2222U);
  set_half64(cpu, 1, 0, 0x4444'4444'4444'4444U);
  set_half64(cpu, 1, 1, 0x4545'4545'4545'4545U);
  set_half64(cpu, 2, 0, std::uint64_t{one} << 32 | one);               // d4
  set_half64(cpu, 2, 1, std::uint64_t{three_quarters_ulp} << 32 |
                            three_quarters_ulp);                        // d5
  for (const unsigned q : {3U, 4U, 5U, 6U})
    for (unsigned lane = 0; lane < 4; ++lane)
      set_lane32(cpu, q, lane, sentinel);
  return cpu;
}

void floating_point_environment() {
  Fixture fixture{0x1a32'0d01};
  load_fp_program(fixture);
  for (const bool toward_zero : {true, false}) {
    const std::uint32_t vfp_sum = toward_zero ? one : one_ulp_up;
    for (const bool zero : {false, true}) {
      // The second round of each pair runs the chained translations.
      for (unsigned round = 0; round < 2; ++round) {
        auto cpu = fp_task(toward_zero ? round_toward_zero : 0U, zero);
        const auto result = fixture.run(cpu);
        require(result.reason == ExitReason::system_call);
        require(lane32(cpu, 0, 0) == vfp_sum);
        // The Neon sums are nearest whatever the guest's rounding mode.
        require(half64(cpu, 1, 1) ==
                (std::uint64_t{one_ulp_up} << 32 | one_ulp_up));
        require(half64(cpu, 1, 0) == 0x4444'4444'4444'4444U);
        require(half64(cpu, 3, 1) ==
                (std::uint64_t{one_ulp_up} << 32 | one_ulp_up));
        require(half64(cpu, 3, 0) == (std::uint64_t{sentinel} << 32 | sentinel));
        require(lane32(cpu, 4, 0) == (zero ? sentinel : vfp_sum));
        require(lane32(cpu, 5, 0) == (zero ? vfp_sum : sentinel));
        require(lane32(cpu, 6, 0) == vfp_sum);
        // The guest's FPCR, not the Standard value, is what an exit leaves.
        require(cpu.fpcr == (toward_zero ? round_toward_zero : 0U));
        require((cpu.fpsr & fpscr_qc) == 0U);
      }
    }
  }
}

/* The two kinds of block alternate in a loop: each ends where the other
 * begins, through a chained branch and, every 4096 instructions, through the
 * executor, which leaves the host FPCR as the last block set it. The result
 * of each kind shows the FPCR it ran under whatever the order. */
void floating_point_modes_across_blocks() {
  constexpr std::uint32_t vfp_add = 0xee30'0a81U;  // vadd.f32 s0, s1, s2
  constexpr std::uint32_t neon_add = 0xf204'3d05U; // vadd.f32 d3, d4, d5
  for (const bool neon_first : {true, false}) {
    Fixture fixture{neon_first ? 0x1a32'0d03U : 0x1a32'0d04U};
    fixture.code32(0, {
        0xeee1'1a10U,                       // vmsr fpscr, r1
        neon_first ? neon_add : vfp_add,
        0xe252'2001U,                       // subs r2, r2, #1
        0x1a00'0000U,                       // bne +0 (the second block)
        0xea00'0001U,                       // b +1 (the svc)
        neon_first ? vfp_add : neon_add,
        0xeaff'fff9U,                       // b -7 (the first block)
        arm_svc});
    for (const std::uint32_t laps : {2U, 3U, 4095U, 4096U, 5000U, 5001U}) {
      auto cpu = fp_task(round_toward_zero, false);
      cpu.registers[2] = laps;
      const auto result = fixture.run(cpu, 200'000);
      require(result.reason == ExitReason::system_call);
      require(cpu.registers[2] == 0U);
      // VFP: the guest's round toward zero. Neon: nearest.
      require(lane32(cpu, 0, 0) == one);
      require(half64(cpu, 1, 1) ==
              (std::uint64_t{one_ulp_up} << 32 | one_ulp_up));
      require(cpu.fpcr == round_toward_zero);
    }
  }
}

/* VMAX and VMIN (single precision, D and Q) run under the guest's FPCR when no
 * operand is a nonzero denormal or a NaN, after a VFP instruction and as the
 * first floating-point instruction of a block. Whatever the operands and the
 * guest's FPCR, their results and the cumulative flags are those of the same
 * instructions run after an Advanced SIMD one, under the Standard FPSCR
 * value. */
void minmax_between_modes() {
  Fixture fixture{0x1a32'0d05};
  const std::uint32_t vmax_d = 0xf204'3f05U; // vmax.f32 d3, d4, d5
  const std::uint32_t vmin_d = 0xf224'6f05U; // vmin.f32 d6, d4, d5
  const std::uint32_t vmax_q = 0xf20a'8f4cU; // vmax.f32 q4, q5, q6
  const std::uint32_t vmin_q = 0xf22a'ef4cU; // vmin.f32 q7, q5, q6
  const std::uint32_t vfp_add = 0xee30'0a81U; // vadd.f32 s0, s1, s2
  const std::uint32_t neon_add = 0xf24d'cdadU; // vadd.f32 d28, d29, d29
  const std::array<std::size_t, 3> offsets{0, 0x100, 0x200};
  fixture.code32(offsets[0], {vfp_add, vmax_d, vmin_d, vmax_q, vmin_q, vfp_add,
                              arm_svc});
  fixture.code32(offsets[1], {neon_add, vmax_d, vmin_d, vmax_q, vmin_q,
                              neon_add, arm_svc});
  fixture.code32(offsets[2], {vmax_d, vmin_d, vmax_q, vmin_q, vfp_add, arm_svc});
  const std::array<std::uint32_t, 16> values{
      0x0000'0000U, 0x8000'0000U, 0x3f80'0000U, 0xbfc0'0000U, 0x7f7f'ffffU,
      0x7f80'0000U, 0xff80'0000U, 0x0001'2345U, 0x8001'2345U, 0x7fc0'0001U,
      0x7f80'0001U, 0xffc1'2345U, 0x0080'0000U, 0x007f'ffffU, 0x0000'0001U,
      0x4000'0000U};
  std::uint64_t state = 0x9e37'79b9'7f4a'7c15ULL;
  const auto pick = [&] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return values[state % values.size()];
  };
  for (const std::uint32_t fpscr :
       {0x0000'0000U, 0x0300'0000U, round_toward_zero, 0x0080'0000U}) {
    for (unsigned round = 0; round < 400; ++round) {
      std::array<std::uint32_t, 12> lanes{};
      for (auto &lane : lanes)
        lane = round < 16 ? values[round] : pick();
      arm64::CpuState result[3];
      for (unsigned run = 0; run < 3; ++run) {
        auto cpu = Fixture::task(false, offsets[run]);
        cpu.fpcr = fpscr;
        set_lane32(cpu, 0, 1, one);
        set_lane32(cpu, 0, 2, one);
        for (unsigned lane = 0; lane < 4; ++lane) {
          set_lane32(cpu, 2, lane, lanes[lane]);      // d4, d5
          set_lane32(cpu, 5, lane, lanes[4 + lane]);  // q5
          set_lane32(cpu, 6, lane, lanes[8 + lane]);  // q6
          if (round >= 16 && round % 3 == 0)
            set_lane32(cpu, 2, lane, lanes[(lane + 5) % 12]);
        }
        const auto outcome = fixture.run(cpu);
        require(outcome.reason == ExitReason::system_call);
        result[run] = cpu;
      }
      for (const unsigned run : {0U, 2U}) {
        for (const unsigned q : {1U, 3U, 4U, 7U})
          require(std::memcmp(result[run].vectors[q].bytes.data(),
                              result[1].vectors[q].bytes.data(), 16) == 0);
        require(result[run].fpsr == result[1].fpsr);
        require(result[run].fpcr == result[1].fpcr);
      }
    }
  }
}

/* VADD, VSUB, VMUL, VMLA and VMLS (single precision, D and Q) between VFP
 * instructions, after one, and as the first floating-point instruction of a
 * block run under the guest's FPCR behind a test of their operands and
 * results. Whatever the operands and the guest's FPCR (a rounding mode,
 * flush-to-zero, default NaN), the registers and the cumulative flags are
 * those of the same instruction among Advanced SIMD ones, which run under the
 * Standard FPSCR value. */
void float_arithmetic_between_modes() {
  Fixture fixture{0x1a32'0d06, 24};
  struct Op final {
    std::uint32_t word;
    unsigned destination; // the Q register of the result
  };
  const std::array<Op, 10> ops{{
      {0xf204'3d05U, 1}, // vadd.f32 d3, d4, d5
      {0xf224'3d05U, 1}, // vsub.f32 d3, d4, d5
      {0xf304'3d15U, 1}, // vmul.f32 d3, d4, d5
      {0xf204'3d15U, 1}, // vmla.f32 d3, d4, d5
      {0xf224'3d15U, 1}, // vmls.f32 d3, d4, d5
      {0xf20a'8d4cU, 4}, // vadd.f32 q4, q5, q6
      {0xf22a'8d4cU, 4}, // vsub.f32 q4, q5, q6
      {0xf30a'8d5cU, 4}, // vmul.f32 q4, q5, q6
      {0xf20a'8d5cU, 4}, // vmla.f32 q4, q5, q6
      {0xf22a'8d5cU, 4}, // vmls.f32 q4, q5, q6
  }};
  const std::uint32_t vfp_add = 0xee30'0a81U;  // vadd.f32 s0, s1, s2
  const std::uint32_t neon_add = 0xf20f'ed0fU; // vadd.f32 d14, d15, d15
  // Per op and repeat count: among VFP instructions, first in the block, last
  // in the block, and (the reference) among Advanced SIMD ones.
  constexpr std::array<unsigned, 4> repeats{1, 2, 8, 10};
  constexpr std::size_t contexts = 4;
  constexpr std::size_t stride = 0x100;
  std::size_t slot = 0;
  std::array<std::array<std::array<std::size_t, contexts>, repeats.size()>,
             ops.size()>
      offsets{};
  for (std::size_t op = 0; op < ops.size(); ++op) {
    for (std::size_t r = 0; r < repeats.size(); ++r) {
      std::vector<std::uint32_t> body(repeats[r], ops[op].word);
      const auto program = [&](bool lead, bool trail, std::uint32_t filler) {
        std::vector<std::uint32_t> words;
        if (lead)
          words.push_back(filler);
        words.insert(words.end(), body.begin(), body.end());
        if (trail)
          words.push_back(filler);
        words.push_back(arm_svc);
        return words;
      };
      const std::array<std::vector<std::uint32_t>, contexts> programs{{
          program(true, true, vfp_add),
          program(false, true, vfp_add),
          program(true, false, vfp_add),
          program(true, true, neon_add),
      }};
      for (std::size_t c = 0; c < contexts; ++c) {
        offsets[op][r][c] = slot++ * stride;
        for (std::size_t i = 0; i < programs[c].size(); ++i)
          fixture.code32(offsets[op][r][c] + 4 * i, {programs[c][i]});
      }
    }
  }
  const std::array<std::uint32_t, 24> values{
      0x0000'0000U, 0x8000'0000U, 0x3f80'0000U, 0xbfc0'0000U, 0x7f7f'ffffU,
      0x7f80'0000U, 0xff80'0000U, 0x0001'2345U, 0x8001'2345U, 0x7fc0'0001U,
      0x7f80'0001U, 0xffc1'2345U, 0x0080'0000U, 0x007f'ffffU, 0x0000'0001U,
      0x4000'0000U, 0x0080'0001U, 0x0100'0000U, 0x1e00'0000U, 0x1f80'0000U,
      0x4b00'0000U, 0x3400'0000U, 0x5f00'0000U, 0x0180'0000U};
  std::uint64_t state = 0x2545'f491'4f6c'dd1dULL;
  const auto pick = [&] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return values[state % values.size()];
  };
  for (const std::uint32_t fpscr :
       {0x0000'0000U, 0x0300'0000U, round_toward_zero, 0x0080'0000U,
        0x0040'0000U, 0x0100'0000U, 0x0200'0000U, 0x03c0'0000U}) {
    for (unsigned round = 0; round < 250; ++round) {
      std::array<std::uint32_t, 16> lanes{};
      for (auto &lane : lanes)
        lane = round < values.size() ? values[round] : pick();
      if (round >= values.size() && round % 4 == 0)
        for (auto &lane : lanes)
          lane = (lane & 0x8000'0000U) | (0x3f00'0000U + (lane & 0x00ff'ffffU));
      for (std::size_t op = 0; op < ops.size(); ++op)
      for (std::size_t r = 0; r < repeats.size(); ++r) {
        std::array<arm64::CpuState, contexts> result{};
        for (std::size_t c = 0; c < contexts; ++c) {
          auto cpu = Fixture::task(false, offsets[op][r][c]);
          cpu.fpcr = fpscr;
          set_lane32(cpu, 0, 1, one);
          set_lane32(cpu, 0, 2, one);
          set_lane32(cpu, 7, 0, one);
          set_lane32(cpu, 7, 1, one);
          set_lane32(cpu, 7, 2, one);
          set_lane32(cpu, 7, 3, one);
          for (unsigned lane = 0; lane < 4; ++lane) {
            set_lane32(cpu, 2, lane, lanes[lane]);       // d4, d5
            set_lane32(cpu, 5, lane, lanes[4 + lane]);   // q5
            set_lane32(cpu, 6, lane, lanes[8 + lane]);   // q6
            set_lane32(cpu, 4, lane, lanes[12 + lane]);  // q4
          }
          set_lane32(cpu, 1, 2, lanes[13]); // d3
          set_lane32(cpu, 1, 3, lanes[14]);
          const auto outcome = fixture.run(cpu);
          require(outcome.reason == ExitReason::system_call);
          result[c] = cpu;
        }
        for (std::size_t c = 0; c + 1 < contexts; ++c) {
          for (const unsigned q : {1U, 2U, 3U, 4U, 5U, 6U})
            if (std::memcmp(result[c].vectors[q].bytes.data(),
                            result[3].vectors[q].bytes.data(), 16) != 0) {
              std::fprintf(stderr,
                           "op %08x x%u ctx %zu fpscr %08x q%u differs: round %u\n",
                           ops[op].word, repeats[r], c, fpscr, q, round);
              for (unsigned lane = 0; lane < 4; ++lane)
                std::fprintf(stderr, "  lane %u: n %08x m %08x old %08x\n", lane,
                             lanes[lane], lanes[4 + lane], lanes[12 + lane]);
              std::abort();
            }
          if (result[c].fpsr != result[3].fpsr) {
            std::fprintf(stderr,
                         "op %08x x%u ctx %zu fpscr %08x fpsr %08x vs %08x: round %u\n",
                         ops[op].word, repeats[r], c, fpscr,
                         static_cast<unsigned>(result[c].fpsr),
                         static_cast<unsigned>(result[3].fpsr), round);
            for (unsigned lane = 0; lane < 4; ++lane)
              std::fprintf(stderr, "  lane %u: n %08x m %08x old %08x\n", lane,
                           lanes[lane], lanes[4 + lane], lanes[12 + lane]);
            std::abort();
          }
          require(result[c].fpcr == result[3].fpcr);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// The cumulative saturation flag.

void saturation_flag() {
  Fixture fixture{0x1a32'0d02};
  fixture.code32(0, {
      0xf287'0e1fU, // vmov.i8 d0, #0x7f
      0xf280'1e11U, // vmov.i8 d1, #1
      0xf201'2011U, // vqadd.s8 d2, d1, d1
      0xeef1'0a10U, // vmrs r0, fpscr
      0xf200'3011U, // vqadd.s8 d3, d0, d1
      0xeef1'1a10U, // vmrs r1, fpscr
      arm_svc});
  auto cpu = Fixture::task(false);
  auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[0] == 0U);
  require(cpu.registers[1] == fpscr_qc);
  require(half64(cpu, 1, 1) == 0x7f7f'7f7f'7f7f'7f7fU); // d3
  require(half64(cpu, 1, 0) == 0x0202'0202'0202'0202U); // d2
  require((cpu.fpsr & fpscr_qc) != 0U);

  // The flag is cumulative: a run that starts with it set keeps it, and
  // reads it before it saturates.
  cpu = Fixture::task(false);
  cpu.fpsr = fpscr_qc;
  result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[0] == fpscr_qc);
  require((cpu.fpsr & fpscr_qc) != 0U);
}

// ---------------------------------------------------------------------------
// Vector memory.

constexpr std::size_t source_offset = 0x0;
constexpr std::size_t copy_offset = 0x400;
constexpr std::size_t structure_offset = 0x100;
constexpr std::size_t result_offset = 0x500;

std::uint8_t pattern(std::size_t index) {
  return static_cast<std::uint8_t>(index * 7U + 3U);
}

void fill_pattern(Fixture &fixture, std::size_t bytes) {
  std::vector<std::byte> data(bytes);
  for (std::size_t index = 0; index < bytes; ++index)
    data[index] = static_cast<std::byte>(pattern(index));
  fixture.data_bytes(0, data);
}

void load_memory_program(Fixture &fixture) {
  fixture.code32(0, {
      0xecb0'0b08U, // vldmia r0!, {d0-d3}
      0xeca1'0b08U, // vstmia r1!, {d0-d3}
      0xf422'400dU, // vld4.8 {d4-d7}, [r2]!
      0xf405'420fU, // vst1.8 {d4-d7}, [r5]
      0xed10'8b02U, // vldr d8, [r0, #-8]
      0xed01'aa01U, // vstr s20, [r1, #-4]
      arm_svc});
}

arm64::CpuState memory_task(std::size_t structure_source) {
  auto cpu = Fixture::task(false);
  cpu.registers[0] = data_base + source_offset;
  cpu.registers[1] = data_base + copy_offset;
  cpu.registers[2] = data_base + static_cast<std::uint32_t>(structure_source);
  cpu.registers[5] = data_base + result_offset;
  for (unsigned lane = 0; lane < 4; ++lane)
    set_lane32(cpu, 5, lane, 0xa5a5'a5a5U);
  return cpu;
}

/* The program's effect on memory and registers, from the pattern. */
void check_memory_program(Fixture &fixture, const arm64::CpuState &cpu,
                          std::size_t structure_source) {
  const auto copied = fixture.read_data(copy_offset, 32);
  for (std::size_t index = 0; index < 28; ++index)
    require(static_cast<std::uint8_t>(copied[index]) == pattern(index));
  // The last word is the vstr of s20 over the end of the block store.
  require(fixture.read32(copy_offset + 28) == 0xa5a5'a5a5U);
  require(cpu.registers[0] == data_base + 32U);
  require(cpu.registers[1] == data_base + copy_offset + 32U);
  require(cpu.registers[2] == data_base + structure_source + 32U);
  // vld4.8 de-interleaves four byte structures into d4-d7; vst1.8 writes them
  // out in register order.
  const auto stored = fixture.read_data(result_offset, 32);
  for (std::size_t reg = 0; reg < 4; ++reg)
    for (std::size_t element = 0; element < 8; ++element)
      require(static_cast<std::uint8_t>(stored[8 * reg + element]) ==
              pattern(structure_source + 4 * element + reg));
  // vldr d8, [r0, #-8] loads the block's last doubleword.
  std::uint64_t last = 0;
  for (std::size_t index = 0; index < 8; ++index)
    last |= std::uint64_t{pattern(24 + index)} << (8 * index);
  require(half64(cpu, 4, 0) == last);
}

void vector_memory_stays_inline() {
  Fixture fixture{0x1a32'0d03};
  load_memory_program(fixture);
  fill_pattern(fixture, 2 * guest_page);
  auto cpu = memory_task(structure_offset);
  ExitCount exits;
  const auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == 7);
  require(ExitCount::total() == 0);
  check_memory_program(fixture, cpu, structure_offset);
}

void vector_memory_across_a_page() {
  Fixture fixture{0x1a32'0d04};
  load_memory_program(fixture);
  fill_pattern(fixture, 2 * guest_page);
  // The 32-byte structure load starts 16 bytes before the end of the page.
  const std::size_t crossing = guest_page - 16;
  auto cpu = memory_task(crossing);
  const auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == 7);
  check_memory_program(fixture, cpu, crossing);
}

// ---------------------------------------------------------------------------
// Loads that cross a page: through the first page's mapping where the two are
// adjacent in the host, through the staging area where they are not, and in the
// library where either page is not in the TLB.

struct CrossingLoad final {
  std::uint32_t word;
  unsigned bytes;
  int offset;        // the immediate added to the base (negative: subtracted)
  unsigned multiple; // the alignment of the address the form needs
};

constexpr std::array<CrossingLoad, 22> crossing_loads{{
    {0xf422'070fU, 8, 0, 1},   // vld1.8 {d0}, [r2]
    {0xf422'0a4fU, 16, 0, 1},  // vld1.16 {d0, d1}, [r2]
    {0xf422'068fU, 24, 0, 1},  // vld1.32 {d0-d2}, [r2]
    {0xf422'02cfU, 32, 0, 1},  // vld1.64 {d0-d3}, [r2]
    {0xf422'020dU, 32, 0, 1},  // vld1.8 {d0-d3}, [r2]!
    {0xf422'080fU, 16, 0, 1},  // vld2.8 {d0, d1}, [r2]
    {0xf422'034fU, 32, 0, 1},  // vld2.16 {d0-d3}, [r2]
    {0xf422'040fU, 24, 0, 1},  // vld3.8 {d0-d2}, [r2]
    {0xf422'0043U, 32, 0, 1},  // vld4.16 {d0-d3}, [r2], r3
    {0xf422'000fU, 32, 0, 1},  // vld4.8 {d0-d3}, [r2]
    {0xf4a2'088fU, 4, 0, 1},   // vld1.32 {d0[1]}, [r2]
    {0xf4a2'054fU, 4, 0, 1},   // vld2.16 {d0[1], d1[1]}, [r2]
    {0xf4a2'036fU, 4, 0, 1},   // vld4.8 {d0[3]-d3[3]}, [r2]
    {0xf4a2'0c8fU, 4, 0, 1},   // vld1.32 {d0[]}, [r2]
    {0xf4a2'0c6fU, 4, 0, 1},   // vld1.16 {d0[], d1[]}, [r2]
    {0xf4a2'0e0fU, 3, 0, 1},   // vld3.8 {d0[], d1[], d2[]}, [r2]
    {0xed92'0b00U, 8, 0, 4},   // vldr d0, [r2]
    {0xed92'1b02U, 8, 8, 4},   // vldr d1, [r2, #8]
    {0xec92'0b08U, 32, 0, 4},  // vldmia r2, {d0-d3}
    {0xecb2'0a08U, 32, 0, 4},  // vldmia r2!, {s0-s7}
    {0xec92'0b0aU, 40, 0, 4},  // vldmia r2, {d0-d4}: past the staged limit
    {0xed32'0b04U, 16, -16, 4} // vldmdb r2!, {d0, d1}
}};

void vector_load_across_a_page() {
  Fixture fixture{0x1a32'0d0fU};
  fill_pattern(fixture, 2 * guest_page);
  // Adjacent in the guest and the host, and adjacent in the guest only.
  constexpr std::uint32_t split_base = data_base + 0x40'0000U;
  fixture.map_data_alias(split_base, 1);
  fixture.map_data_alias(split_base + static_cast<std::uint32_t>(guest_page), 0);
  const std::uint32_t reference_address = data_base + 0x200U;
  for (const bool split : {false, true}) {
    const std::uint32_t boundary =
        split ? split_base + static_cast<std::uint32_t>(guest_page)
              : data_base + static_cast<std::uint32_t>(guest_page);
    // The byte the guest sees `distance` bytes from the boundary: before it
    // (negative) and after it.
    const auto seen = [&](int distance) -> std::uint8_t {
      if (!split)
        return fixture.read8(guest_page + static_cast<std::size_t>(distance));
      return distance < 0
                 ? fixture.read8(2 * guest_page + static_cast<std::size_t>(distance))
                 : fixture.read8(static_cast<std::size_t>(distance));
    };
    for (const auto &form : crossing_loads) {
      fixture.code32(0, {form.word, arm_svc});
      for (unsigned k = form.multiple; k < form.bytes; k += form.multiple) {
        // `k` bytes before the boundary, the form's bytes in all.
        std::array<std::byte, 64> bytes{};
        for (unsigned j = 0; j < form.bytes; ++j)
          bytes[j] = static_cast<std::byte>(seen(static_cast<int>(j) - static_cast<int>(k)));
        fixture.data_bytes(0x200, std::span{bytes}.first(form.bytes));
        const auto run = [&](std::uint32_t address, unsigned repeats) {
          arm64::CpuState cpu{};
          for (unsigned round = 0; round < repeats; ++round) {
            cpu = Fixture::task(false);
            cpu.registers[2] = address - static_cast<std::uint32_t>(form.offset);
            cpu.registers[3] = 24U;
            for (unsigned q = 0; q < 4; ++q)
              for (unsigned lane = 0; lane < 4; ++lane)
                set_lane32(cpu, q, lane, 0xa5a5'0000U + 16U * q + lane);
            const auto result = fixture.run(cpu);
            require(result.reason == ExitReason::system_call);
          }
          return cpu;
        };
        const auto expected = run(reference_address, 1);
        arm64::CpuState got{};
        {
          // Twice to fill the TLB; the third run is the one that is timed.
          got = run(boundary - k, 2);
          ExitCount exits;
          got = run(boundary - k, 1);
          if (form.bytes <= 32U)
            require(ExitCount::total() == 0);
        }
        for (unsigned q = 0; q < 4; ++q)
          require(std::memcmp(got.vectors[q].bytes.data(),
                              expected.vectors[q].bytes.data(), 16) == 0);
        require(got.registers[2] - (boundary - k) ==
                expected.registers[2] - reference_address);
      }
    }
  }
}

/* A memory transfer the library runs (a store across a page) leaves the
 * guest's native floating-point environment installed: VFP instructions
 * before and after it round toward zero, and the host's own rounding is
 * nearest once the run is over. */
void library_memory_transfer_keeps_the_fp_environment() {
  Fixture fixture{0x1a32'0d07};
  fill_pattern(fixture, 2 * guest_page);
  fixture.code32(0, {
      0xeee1'1a10U, // vmsr fpscr, r1
      0xee30'0a81U, // vadd.f32 s0, s1, s2     (round toward zero)
      0xf442'8a8fU, // vst1.32 {d24, d25}, [r2] (across a page: the library)
      0xee30'ca81U, // vadd.f32 s24, s1, s2    (the guest's FPCR again)
      0xf204'7d05U, // vadd.f32 d7, d4, d5     (Neon: nearest)
      0xee30'ea81U, // vadd.f32 s28, s1, s2
      arm_svc});
  const std::size_t crossing = guest_page - 8;
  for (const std::uint32_t rounding : {round_toward_zero, 0U}) {
    for (unsigned round = 0; round < 3; ++round) {
      auto cpu = fp_task(rounding, false);
      cpu.registers[2] = data_base + static_cast<std::uint32_t>(crossing);
      for (unsigned lane = 0; lane < 4; ++lane)
        set_lane32(cpu, 12, lane, 0x0102'0304U * (lane + 1U));
      ExitCount exits;
      const auto result = fixture.run(cpu);
      require(result.reason == ExitReason::system_call);
      require(ExitCount::total() != 0);
      const std::uint32_t vfp_sum = rounding != 0U ? one : one_ulp_up;
      require(lane32(cpu, 0, 0) == vfp_sum);
      require(lane32(cpu, 6, 0) == vfp_sum);
      require(lane32(cpu, 7, 0) == vfp_sum);
      require(half64(cpu, 3, 1) ==
              (std::uint64_t{one_ulp_up} << 32 | one_ulp_up));
      require(cpu.fpcr == rounding);
      const auto stored = fixture.read_data(crossing, 16);
      require(std::memcmp(stored.data(), cpu.vectors[12].bytes.data(), 16) == 0);
    }
  }
  // The host's rounding after the runs.
  volatile float base = 1.0F;
  volatile float small = 0x1.8p-24F;
  const float sum = base + small;
  require(sum > 1.0F);
}

/* One instruction at `address`, run from fresh registers; the faulting access
 * must leave the registers it would have written alone. */
void vector_memory_faults() {
  const auto sentinel_registers = [](arm64::CpuState &cpu) {
    set_half64(cpu, 0, 0, 0x0123'4567'89ab'cdefU);
    set_half64(cpu, 0, 1, 0xfedc'ba98'7654'3210U);
  };
  const std::uint32_t unmapped = data_base + 2U * static_cast<std::uint32_t>(guest_page);

  {
    // A load that starts on the last mapped bytes and ends past them faults
    // on the first byte of the page that is not there.
    Fixture fixture{0x1a32'0d05};
    fixture.code32(0, {0xf426'0a0fU, arm_svc}); // vld1.8 {d0, d1}, [r6]
    auto cpu = Fixture::task(false);
    cpu.registers[6] = unmapped - 8U;
    sentinel_registers(cpu);
    const auto result = fixture.run(cpu);
    require(result.reason == ExitReason::memory_fault);
    require(result.retired_instructions == 0);
    require(result.fault.address == unmapped);
    require(result.fault.access == ::uml::memory::MemoryAccess::read);
    require(!result.fault.alignment);
    require(half64(cpu, 0, 0) == 0x0123'4567'89ab'cdefU);
    require(half64(cpu, 0, 1) == 0xfedc'ba98'7654'3210U);
  }
  {
    // A misaligned access that names its alignment.
    Fixture fixture{0x1a32'0d06};
    fixture.code32(0, {0xf426'079fU, arm_svc}); // vld1.32 {d0}, [r6:64]
    auto cpu = Fixture::task(false);
    cpu.registers[6] = data_base + 4U;
    sentinel_registers(cpu);
    const auto result = fixture.run(cpu);
    require(result.reason == ExitReason::memory_fault);
    require(result.retired_instructions == 0);
    require(result.fault.address == data_base + 4U);
    require(result.fault.alignment);
    require(half64(cpu, 0, 0) == 0x0123'4567'89ab'cdefU);
  }
  {
    // A store that would cross into the unmapped page writes no byte.
    Fixture fixture{0x1a32'0d07};
    fixture.code32(0, {0xf406'084fU, arm_svc}); // vst2.16 {d0, d1}, [r6]
    for (std::size_t index = 0; index < 8; ++index)
      fixture.data_bytes(2 * guest_page - 8 + index,
                         std::array<std::byte, 1>{std::byte{0x5a}});
    auto cpu = Fixture::task(false);
    cpu.registers[6] = unmapped - 8U;
    sentinel_registers(cpu);
    const auto result = fixture.run(cpu);
    require(result.reason == ExitReason::memory_fault);
    require(result.retired_instructions == 0);
    require(result.fault.address == unmapped);
    require(result.fault.access == ::uml::memory::MemoryAccess::write);
    for (const auto byte : fixture.read_data(2 * guest_page - 8, 8))
      require(byte == std::byte{0x5a});
  }
}

// ---------------------------------------------------------------------------
// VCMP followed by VMRS APSR_nzcv: the flags go from the comparison to the
// guest's, whether or not the translation passes them through the host NZCV.

constexpr std::uint32_t quiet_nan = 0x7fc0'0000U;
constexpr std::uint32_t positive_infinity = 0x7f80'0000U;
constexpr std::uint32_t denormal_one = 0x0000'0001U;
constexpr std::uint32_t negative_zero = 0x8000'0000U;

/* The FPSCR flags nibble of comparing a with b (VCMP). */
std::uint32_t compare_flags(float a, float b) {
  if (a != a || b != b)
    return 0x3U; // unordered: C, V
  if (a == b)
    return 0x6U; // equal: Z, C
  return a < b ? 0x8U : 0x2U; // less: N; greater: C
}
float as_float(std::uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

void vcmp_flags_reach_the_apsr() {
  const std::uint32_t vcmp_s = 0xeeb4'1a61U;   // vcmp.f32 s2, s3
  const std::uint32_t vcmpe_s = 0xeeb4'1ae1U;  // vcmpe.f32 s2, s3
  const std::uint32_t vcmp_d = 0xeeb4'2b43U;   // vcmp.f64 d2, d3
  const std::uint32_t vcmpne_s = 0x1eb4'1a61U; // vcmpne.f32 s2, s3
  const std::uint32_t vmrs_apsr = 0xeef1'fa10U;
  const std::uint32_t vmrsne_apsr = 0x1ef1'fa10U;
  const std::uint32_t vmrs_r8 = 0xeef1'8a10U;
  const std::uint32_t movlt = 0xb3a0'2001U, movgt = 0xc3a0'3001U,
                      moveq = 0x03a0'4001U, movvs = 0x63a0'5001U,
                      movhs = 0x23a0'6001U, movmi = 0x43a0'7001U;
  const std::uint32_t adds_r9 = 0xe299'9001U; // adds r9, r9, #1
  const auto program = [&](Fixture &fixture, std::size_t offset,
                           std::uint32_t compare, std::uint32_t read,
                           bool between) {
    if (between)
      fixture.code32(offset, {compare, adds_r9, read, movlt, movgt, moveq,
                              movvs, movhs, movmi, vmrs_r8, arm_svc});
    else
      fixture.code32(offset, {compare, read, movlt, movgt, moveq, movvs, movhs,
                              movmi, vmrs_r8, arm_svc});
  };
  struct Pair final { std::uint32_t a, b; };
  const std::array<Pair, 10> pairs{{{one, 0x4000'0000U},
                                    {0x4000'0000U, one},
                                    {one, one},
                                    {quiet_nan, one},
                                    {one, quiet_nan},
                                    {positive_infinity, positive_infinity},
                                    {negative_zero, 0U},
                                    {denormal_one, 0U},
                                    {0xbf80'0000U, one},
                                    {quiet_nan, quiet_nan}}};
  // Context 0: adjacent VCMP and VMRS; 1: VCMPE; 2: an instruction between;
  // 3: a conditional VMRS; 4: a conditional VCMP; 5: VCMP.F64.
  for (unsigned context = 0; context < 6; ++context) {
    Fixture fixture{0x1a32'0d09U + context};
    switch (context) {
    case 0: program(fixture, 0, vcmp_s, vmrs_apsr, false); break;
    case 1: program(fixture, 0, vcmpe_s, vmrs_apsr, false); break;
    case 2: program(fixture, 0, vcmp_s, vmrs_apsr, true); break;
    case 3: program(fixture, 0, vcmp_s, vmrsne_apsr, false); break;
    case 4: program(fixture, 0, vcmpne_s, vmrs_apsr, false); break;
    default: program(fixture, 0, vcmp_d, vmrs_apsr, false); break;
    }
    for (const auto &pair : pairs) {
      for (const std::uint32_t before : {0U, flag_z, flag_n | 0x2000'0000U}) {
        // Twice: the second run is the chained translation.
        for (unsigned round = 0; round < 2; ++round) {
          auto cpu = Fixture::task(false);
          cpu.nzcv = before;
          set_lane32(cpu, 0, 2, pair.a);
          set_lane32(cpu, 0, 3, pair.b);
          set_half64(cpu, 1, 0, std::bit_cast<std::uint64_t>(
                                    static_cast<double>(as_float(pair.a))));
          set_half64(cpu, 1, 1, std::bit_cast<std::uint64_t>(
                                    static_cast<double>(as_float(pair.b))));
          const auto result = fixture.run(cpu);
          require(result.reason == ExitReason::system_call);
          const std::uint32_t flags =
              compare_flags(as_float(pair.a), as_float(pair.b));
          // Whether the comparison ran, and whether its flags were read.
          const bool zero_before = (before & flag_z) != 0U;
          const bool compared = context != 4 || !zero_before;
          const bool read = context != 3 || !zero_before;
          const std::uint32_t fpscr_flags = compared ? flags : 0U;
          const std::uint32_t guest_flags = read ? fpscr_flags : before >> 28;
          require(cpu.registers[8] >> 28 == fpscr_flags);
          require(cpu.nzcv >> 28 == guest_flags);
          const bool n = (guest_flags & 8U) != 0U, z = (guest_flags & 4U) != 0U,
                     c = (guest_flags & 2U) != 0U, v = (guest_flags & 1U) != 0U;
          require(cpu.registers[2] == (n != v ? 1U : 0U));
          require(cpu.registers[3] == (!z && n == v ? 1U : 0U));
          require(cpu.registers[4] == (z ? 1U : 0U));
          require(cpu.registers[5] == (v ? 1U : 0U));
          require(cpu.registers[6] == (c ? 1U : 0U));
          require(cpu.registers[7] == (n ? 1U : 0U));
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// A run of single-precision VFP instructions keeps the S registers it reads
// and writes in host registers between its instructions (the S shadows of
// a32_compiler_vector.cpp), forgotten where an instruction may change the S
// register another way or use the host register: a double-precision or
// Advanced SIMD instruction, a conditional one. An access keeps them: its
// TLB miss preserves the registers. Random programs of such runs among
// integer arithmetic, loads and stores (some across a page), VLDR and VSTR,
// VMOV to and from core registers and comparisons read by VMRS, some of them
// counted loops, run whole and one instruction at a time, must leave the same
// registers, vectors, FPSCR and memory. Each program has an address space of
// its own, so its first accesses miss in the TLB inside a translation.

class Xorshift final {
public:
  explicit Xorshift(std::uint64_t seed) : state_{seed | 1U} {}
  std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }
  unsigned below(unsigned bound) {
    return static_cast<unsigned>(next() % bound);
  }

private:
  std::uint64_t state_;
};

/* An unconditional VFP data-processing word on S d, S n and S m. */
std::uint32_t vfp_single(std::uint32_t base, unsigned d, unsigned n,
                         unsigned m) {
  return 0xe000'0000U | base | (d & 1U) << 22 | (n >> 1) << 16 |
         (d >> 1) << 12 | (n & 1U) << 7 | (m & 1U) << 5 | m >> 1;
}

std::vector<std::uint32_t> single_run_program(Xorshift &random, bool loop) {
  /* VADD, VSUB, VMUL, VNMUL, VDIV, VMLA, VMLS, VNMLS, VNMLA, VFMA, VFMS,
   * VFNMS and VFNMA. */
  static constexpr std::uint32_t three[] = {
      0x0e30'0a00U, 0x0e30'0a40U, 0x0e20'0a00U, 0x0e20'0a40U, 0x0e80'0a00U,
      0x0e00'0a00U, 0x0e00'0a40U, 0x0e10'0a00U, 0x0e10'0a40U, 0x0ea0'0a00U,
      0x0ea0'0a40U, 0x0e90'0a00U, 0x0e90'0a40U};
  /* VABS, VNEG, VSQRT, VMOV, VCVT.F32.S32, VCVT.F32.U32, VCVT.S32.F32,
   * VCVTR.S32.F32, VCVT.U32.F32 and VCVTR.U32.F32. */
  static constexpr std::uint32_t two[] = {
      0x0eb0'0ac0U, 0x0eb1'0a40U, 0x0eb1'0ac0U, 0x0eb0'0a40U, 0x0eb8'0ac0U,
      0x0eb8'0a40U, 0x0ebd'0ac0U, 0x0ebd'0a40U, 0x0ebc'0ac0U, 0x0ebc'0a40U};
  const auto pick = [&](const auto &table) {
    return table[random.below(static_cast<unsigned>(std::size(table)))];
  };
  const auto s = [&] { return random.below(20); };
  const auto r = [&] { return random.below(8); };
  std::vector<std::uint32_t> words{
      0xeee1'1a10U,                          // vmsr fpscr, r1
      0xe3a0'9000U | (2U + random.below(2)), // mov r9, #2 or #3
  };
  const std::size_t body = words.size();
  const unsigned length = 8U + random.below(70);
  for (unsigned i = 0; i < length; ++i) {
    const unsigned kind = random.below(100);
    if (kind < 34) {
      words.push_back(vfp_single(pick(three), s(), s(), s()));
    } else if (kind < 50) {
      words.push_back(vfp_single(pick(two), s(), 0, s()));
    } else if (kind < 56) {
      const unsigned n = s();
      // vmov Sn, Rt or vmov Rt, Sn.
      words.push_back((random.below(2) != 0 ? 0xee00'0a10U : 0xee10'0a10U) |
                      (n >> 1) << 16 | r() << 12 | (n & 1U) << 7);
    } else if (kind < 59) {
      const unsigned d = s(), imm8 = random.below(256);
      words.push_back(0xeeb0'0a00U | (d & 1U) << 22 | (imm8 >> 4) << 16 |
                      (d >> 1) << 12 | (imm8 & 15U)); // vmov.f32 Sd, #imm
    } else if (kind < 63) {
      // vcmp or vcmpe, vmrs APSR_nzcv, fpscr and movgt r8, #1.
      words.push_back(vfp_single(
          random.below(2) != 0 ? 0x0eb4'0a40U : 0x0eb4'0ac0U, s(), 0, s()));
      words.push_back(0xeef1'fa10U);
      words.push_back(0xc3a0'8001U);
    } else if (kind < 70) {
      const unsigned d = s();
      // vldr or vstr Sd, [r10, #4 * imm8]
      words.push_back((random.below(3) != 0 ? 0xed9a'0a00U : 0xed8a'0a00U) |
                      (d & 1U) << 22 | (d >> 1) << 12 | random.below(64));
    } else if (kind < 76) {
      // add or mla.
      if (random.below(2) != 0)
        words.push_back(0xe080'0000U | r() << 16 | r() << 12 | r());
      else
        words.push_back(0xe020'0090U | r() << 16 | r() << 12 | r() << 8 |
                        r());
    } else if (kind < 82) {
      // ldr or str Rt, [r11, #imm]: r11 is 64 bytes before a page end.
      words.push_back((random.below(2) != 0 ? 0xe59b'0000U : 0xe58b'0000U) |
                      r() << 12 |
                      (random.below(2) != 0 ? 60U + random.below(4)
                                            : random.below(128)));
    } else if (kind < 85) {
      // vadd.f64 Dd, Dn, Dm
      const unsigned d = random.below(10), n = random.below(10);
      words.push_back(0xee30'0b00U | n << 16 | d << 12 | random.below(10));
    } else if (kind < 87) {
      // vcvt.f64.f32 Dd, Sm
      const unsigned d = random.below(10), m = s();
      words.push_back(0xeeb7'0ac0U | d << 12 | (m & 1U) << 5 | m >> 1);
    } else if (kind < 89) {
      // vadd.f32 Qd, Qn, Qm
      const unsigned d = 2 * random.below(5), n = 2 * random.below(5);
      words.push_back(0xf200'0d40U | n << 16 | d << 12 | 2 * random.below(5));
    } else if (kind < 94) {
      // A conditional single-precision operation.
      const std::uint32_t word = vfp_single(pick(three), s(), s(), s());
      words.push_back((word & 0x0fff'ffffU) | random.below(14) << 28);
    } else if (kind < 97) {
      // A conditional add.
      words.push_back(random.below(14) << 28 | 0x0080'0000U | r() << 16 |
                      r() << 12 | r());
    } else {
      words.push_back(0xeee1'ca10U); // vmsr fpscr, r12
    }
  }
  if (loop) {
    words.push_back(0xe259'9001U); // subs r9, r9, #1
    // bne body
    const auto offset = static_cast<std::int32_t>(body) -
                        static_cast<std::int32_t>(words.size() + 2U);
    words.push_back(0x1a00'0000U |
                    (static_cast<std::uint32_t>(offset) & 0x00ff'ffffU));
  }
  words.push_back(arm_svc);
  return words;
}

struct SingleRunOutcome final {
  arm64::CpuState cpu;
  std::size_t retired;
  ExitReason reason;
  std::vector<std::byte> data;
};

void single_run_data(Fixture &fixture, std::uint64_t seed) {
  Xorshift random{seed};
  std::vector<std::byte> data(2 * guest_page);
  for (std::size_t index = 0; index < data.size(); index += 4) {
    const auto word = static_cast<std::uint32_t>(random.next());
    // Half of the words are small floats, the others any bits.
    const std::uint32_t value =
        (word & 1U) != 0U ? 0x3f00'0000U | (word >> 9 & 0x00ff'ffffU) : word;
    std::memcpy(data.data() + index, &value, 4);
  }
  fixture.data_bytes(0, data);
}

SingleRunOutcome single_run(Fixture &fixture, arm64::CpuState cpu,
                            std::uint64_t seed, bool stepped) {
  single_run_data(fixture, seed);
  std::size_t retired = 0;
  ExitReason reason = ExitReason::single_step;
  if (stepped) {
    for (unsigned step = 0; step < 4096; ++step) {
      const auto result = fixture.run(cpu, 1, true);
      retired += result.retired_instructions;
      reason = result.reason;
      if (reason != ExitReason::single_step)
        break;
    }
  } else {
    const auto result = fixture.run(cpu, 4096);
    retired = result.retired_instructions;
    reason = result.reason;
  }
  return {cpu, retired, reason, fixture.read_data(0, 2 * guest_page)};
}

/* The registers a single-precision run starts with: interesting values in
 * most S registers, any bits in the rest. */
arm64::CpuState single_run_task(Xorshift &random) {
  static constexpr std::uint32_t values[] = {
      0x0000'0000U, 0x8000'0000U, 0x3f80'0000U, 0xbfc0'0000U, 0x4020'0000U,
      0x3f00'0000U, 0x5015'02f9U, 0x8020'0000U, 0x7f80'0000U, 0xff80'0000U,
      0x7fc0'0000U, 0x7f80'0001U, 0x47f1'2066U, 0xcf00'0000U, 0x4f80'0000U,
      0x7f7f'ffffU, 0x0000'0007U, 0xffff'fffdU, 0x0000'0001U, 0x4b00'0001U};
  static constexpr std::uint32_t modes[] = {0x0000'0000U, 0x0040'0000U,
                                            0x0080'0000U, 0x00c0'0000U,
                                            0x0100'0000U, 0x0300'0000U};
  auto cpu = Fixture::task(false);
  for (unsigned i = 0; i < 8; ++i)
    cpu.registers[i] = random.below(2) != 0
                           ? values[random.below(std::size(values))]
                           : static_cast<std::uint32_t>(random.next());
  cpu.registers[1] = modes[random.below(std::size(modes))];
  cpu.registers[12] = modes[random.below(std::size(modes))];
  cpu.registers[10] = data_base + 128U;
  cpu.registers[11] = data_base + static_cast<std::uint32_t>(guest_page) - 64U;
  cpu.nzcv = static_cast<std::uint32_t>(random.next()) & 0xf000'0000U;
  for (unsigned q = 0; q < 8; ++q)
    for (unsigned lane = 0; lane < 4; ++lane)
      set_lane32(cpu, q, lane,
                 random.below(3) != 0
                     ? values[random.below(std::size(values))]
                     : static_cast<std::uint32_t>(random.next()));
  return cpu;
}

/* The program in a new address space, run whole and stepped from `rounds`
 * random starts. */
void check_single_run(unsigned long generation,
                      const std::vector<std::uint32_t> &words, Xorshift &random,
                      unsigned rounds, unsigned program) {
  Fixture fixture{generation};
  for (std::size_t i = 0; i < words.size(); ++i)
    fixture.code32(4 * i, {words[i]});
  for (unsigned round = 0; round < rounds; ++round) {
    const auto cpu = single_run_task(random);
    const std::uint64_t seed = random.next();
    const auto whole = single_run(fixture, cpu, seed, false);
    const auto stepped = single_run(fixture, cpu, seed, true);
    bool same = whole.reason == ExitReason::system_call &&
                whole.reason == stepped.reason &&
                whole.retired == stepped.retired &&
                whole.cpu.pc == stepped.cpu.pc &&
                whole.cpu.nzcv >> 28 == stepped.cpu.nzcv >> 28 &&
                whole.cpu.fpsr == stepped.cpu.fpsr &&
                whole.cpu.fpcr == stepped.cpu.fpcr &&
                whole.cpu.a32_fpscr_nzcv == stepped.cpu.a32_fpscr_nzcv &&
                whole.data == stepped.data;
    for (unsigned i = 0; i < 15; ++i)
      same = same && whole.cpu.registers[i] == stepped.cpu.registers[i];
    for (unsigned q = 0; q < 16; ++q)
      same = same && whole.cpu.vectors[q].bytes == stepped.cpu.vectors[q].bytes;
    if (!same) {
      std::fprintf(stderr,
                   "program %u round %u: whole reason=%d retired=%zu "
                   "stepped reason=%d retired=%zu\n",
                   program, round, static_cast<int>(whole.reason),
                   whole.retired, static_cast<int>(stepped.reason),
                   stepped.retired);
      for (const auto word : words)
        std::fprintf(stderr, "  %08x\n", word);
      for (unsigned q = 0; q < 8; ++q)
        for (unsigned lane = 0; lane < 4; ++lane)
          if (lane32(whole.cpu, q, lane) != lane32(stepped.cpu, q, lane))
            std::fprintf(stderr, "  s%u whole=%08x stepped=%08x\n",
                         4 * q + lane, lane32(whole.cpu, q, lane),
                         lane32(stepped.cpu, q, lane));
    }
    require(same);
  }
}

void single_precision_runs_whole_and_stepped() {
  Xorshift random{0x5ade'5a32'f00dU};
  /* Each way an S register with a shadow changes, or an instruction uses
   * the host registers, between a write that shadows S1 (and S5) and reads
   * of it. */
  constexpr std::uint32_t write_s1 = 0xee71'0a82U;  // vadd.f32 s1, s3, s4
  constexpr std::uint32_t write_s5 = 0xee72'2a01U;  // vadd.f32 s5, s4, s2
  constexpr std::uint32_t read_s1 = 0xee30'4aa0U;   // vadd.f32 s8, s1, s1
  constexpr std::uint32_t read_s5 = 0xee72'4a82U;   // vadd.f32 s9, s5, s4
  constexpr std::uint32_t move_out = 0xee10'0a90U;  // vmov r0, s1
  const std::vector<std::uint32_t> changes[] = {
      {0xeef0'0a43U},  // vmov.f32 s1, s6
      {0xeef7'0a08U},  // vmov.f32 s1, #1.5
      {0xee00'2a90U},  // vmov s1, r2
      {0xedda'0a01U},  // vldr s1, [r10, #4]
      {0xeef8'0ac3U},  // vcvt.f32.s32 s1, s6
      {0xeefd'0ae1U},  // vcvt.s32.f32 s1, s3
      {0xee30'0b02U},  // vadd.f64 d0, d0, d2
      {0xf200'0d44U},  // vadd.f32 q0, q0, q2
      {0x1e71'0a82U},  // vaddne.f32 s1, s3, s4
      {0xeeb7'0ac3U},  // vcvt.f64.f32 d0, s6
      {0xe59b'003eU},  // ldr r0, [r11, #62] (across the page: the library)
      {0xe58b'003eU},  // str r0, [r11, #62]
      {0xe59b'0040U},  // ldr r0, [r11, #64] (the second page)
      {0xed8a'0a01U},  // vstr s0, [r10, #4]
      {0xe080'0001U},  // add r0, r0, r1
      {0xeee1'ca10U},  // vmsr fpscr, r12
      {0xeeb4'0a60U, 0xeef1'fa10U}, // vcmp.f32 s0, s1; vmrs APSR_nzcv, fpscr
  };
  /* Loads from both pages around r11 + 64, each the first access to its
   * page in a new address space: a TLB miss, which in the loop comes after
   * S1 has its shadow. */
  constexpr std::uint32_t warm_first = 0xe59b'3000U;  // ldr r3, [r11]
  constexpr std::uint32_t warm_second = 0xe59b'3040U; // ldr r3, [r11, #64]
  unsigned program = 0;
  for (const auto &change : changes) {
    std::vector<std::uint32_t> words{0xeee1'1a10U, warm_first, warm_second,
                                     write_s1, write_s5};
    words.insert(words.end(), change.begin(), change.end());
    words.insert(words.end(), {read_s1, read_s5, move_out, arm_svc});
    check_single_run(0x1a32'5d00U + program, words, random, 4, program);
    ++program;
  }
  /* The same in a counted loop, a host loop from its second pass. */
  for (const auto &change : changes) {
    std::vector<std::uint32_t> words{0xeee1'1a10U, 0xe3a0'9003U, // mov r9, #3
                                     write_s1, warm_first, warm_second,
                                     write_s5};
    words.insert(words.end(), change.begin(), change.end());
    words.insert(words.end(), {read_s1, read_s5, move_out,
                               0xe259'9001U}); // subs r9, r9, #1
    // bne to write_s1
    const auto offset = 2 - static_cast<std::int32_t>(words.size() + 2U);
    words.push_back(0x1a00'0000U |
                    (static_cast<std::uint32_t>(offset) & 0x00ff'ffffU));
    words.push_back(arm_svc);
    check_single_run(0x1a32'5d00U + program, words, random, 4, program);
    ++program;
  }
  constexpr unsigned programs = 240;
  for (unsigned p = 0; p < programs; ++p) {
    const auto words = single_run_program(random, p % 2 == 0);
    check_single_run(0x1a32'5e00U + p, words, random, 3, 100 + p);
  }
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  floating_point_environment();
  floating_point_modes_across_blocks();
  minmax_between_modes();
  float_arithmetic_between_modes();
  saturation_flag();
  vector_memory_stays_inline();
  vector_memory_across_a_page();
  vector_load_across_a_page();
  library_memory_transfer_keeps_the_fp_environment();
  vector_memory_faults();
  vcmp_flags_reach_the_apsr();
  single_precision_runs_whole_and_stepped();
  std::puts("arm64-jit-a32-vector: ok");
  return 0;
}
