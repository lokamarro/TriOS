// SPDX-License-Identifier: GPL-2.0-or-later
/* A32 data processing on the JIT against the reference
 * (arm64_jit_a32_reference.hpp): random sequences of instructions under
 * random conditions and flags. Every parallel add and subtract form also runs
 * against a model of the lanes, and the lowered ones must stay out of the
 * semantic library. The corpus lane carries T32, and the control test the IT
 * blocks. */
#include "arm64_jit_a32_reference.hpp"
#include "arm64_jit_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/aarch32/semantics_exit_counters.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace uml::emulator::tests;
using namespace uml::emulator::tests::a32;
using ::uml::cpu::ExitReason;
namespace aarch32 = ::uml::cpu::arm64::aarch32;

namespace {

class Random final {
public:
  explicit Random(std::uint64_t seed) : state_{seed | 1U} {}
  std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }
  unsigned below(unsigned bound) { return static_cast<unsigned>(next() % bound); }
  std::uint32_t value() {
    static constexpr std::uint32_t special[] = {
        0, 1, 2, 0x7fff'ffffU, 0x8000'0000U, 0xffff'ffffU, 0x8000'0001U,
        0xff, 0x100, 0xffff, 0x1'0000U, 0x7fff'ffffU - 1U, 31, 32, 33, 255};
    switch (below(4)) {
    case 0: return special[below(std::size(special))];
    case 1: return static_cast<std::uint32_t>(next()) >> below(32);
    default: return static_cast<std::uint32_t>(next());
    }
  }

private:
  std::uint64_t state_;
};

struct Instruction final {
  std::uint32_t word;
  bool thumb32;
};

// ---------------------------------------------------------------------------
// A32.

std::uint32_t a32_dp_imm(unsigned cond, ref::DpOp op, bool s, unsigned rd,
                         unsigned rn, unsigned imm12) {
  return cond << 28 | 1U << 25 | static_cast<unsigned>(op) << 21 | (s ? 1U << 20 : 0U) |
         rn << 16 | rd << 12 | imm12;
}
std::uint32_t a32_dp_reg(unsigned cond, ref::DpOp op, bool s, unsigned rd,
                         unsigned rn, unsigned rm, unsigned type,
                         unsigned imm5) {
  return cond << 28 | static_cast<unsigned>(op) << 21 | (s ? 1U << 20 : 0U) |
         rn << 16 | rd << 12 | imm5 << 7 | type << 5 | rm;
}
std::uint32_t a32_dp_rsr(unsigned cond, ref::DpOp op, bool s, unsigned rd,
                         unsigned rn, unsigned rm, unsigned type,
                         unsigned rs) {
  return cond << 28 | static_cast<unsigned>(op) << 21 | (s ? 1U << 20 : 0U) |
         rn << 16 | rd << 12 | rs << 8 | type << 5 | 1U << 4 | rm;
}

void a32_reference_step(ref::State &s, std::uint32_t word) {
  const unsigned cond = word >> 28;
  if (!condition_passed(s, cond))
    return;
  const auto op = static_cast<ref::DpOp>(word >> 21 & 15U);
  const bool set_flags = (word >> 20 & 1U) != 0U;
  const unsigned rn = word >> 16 & 15U;
  const unsigned rd = word >> 12 & 15U;
  ref::Shifted operand2;
  if ((word >> 25 & 1U) != 0U) {
    operand2 = ref::arm_expand_imm_c(word & 0xfffU, s.c);
  } else {
    const unsigned rm = word & 15U;
    const unsigned type = word >> 5 & 3U;
    if ((word >> 4 & 1U) != 0U) {
      const unsigned amount = s.r[word >> 8 & 15U] & 0xffU;
      operand2 = ref::shift_c(s.r[rm], static_cast<ref::Shift>(type), amount, s.c);
    } else {
      unsigned amount = word >> 7 & 31U;
      auto shift = static_cast<ref::Shift>(type);
      if ((type == 1 || type == 2) && amount == 0)
        amount = 32;
      if (type == 3 && amount == 0)
        shift = ref::Shift::rrx;
      operand2 = ref::shift_c(s.r[rm], shift, amount, s.c);
    }
  }
  const auto result =
      ref::data_processing(s, op, s.r[rn], operand2.value, operand2.carry, set_flags);
  if (!ref::dp_tests_only(op))
    s.r[rd] = result;
}

[[nodiscard]] bool state_matches(const arm64::CpuState &cpu,
                                 const ref::State &expected) {
  bool ok = (cpu.nzcv & 0xf000'0000U) == expected.nzcv();
  for (unsigned i = 0; i < 15; ++i)
    ok = ok && cpu.registers[i] == expected.r[i];
  return ok;
}

void report_state(const arm64::CpuState &cpu, const ref::State &expected,
                  const std::vector<std::uint32_t> &program,
                  std::size_t executed, const ref::State &initial,
                  const char *what) {
  std::fprintf(stderr, "MISMATCH (%s) after %zu instructions\n", what, executed);
  std::fprintf(stderr, "  program:");
  for (std::size_t i = 0; i < program.size(); ++i)
    std::fprintf(stderr, " %08x", program[i]);
  std::fprintf(stderr, "\n  initial nzcv=%08x:", initial.nzcv());
  for (unsigned i = 0; i < 15; ++i)
    std::fprintf(stderr, " r%u=%08x", i, initial.r[i]);
  std::fprintf(stderr, "\n  expected nzcv=%08x:", expected.nzcv());
  for (unsigned i = 0; i < 15; ++i)
    std::fprintf(stderr, " r%u=%08x", i, expected.r[i]);
  std::fprintf(stderr, "\n  actual   nzcv=%08x:", cpu.nzcv & 0xf000'0000U);
  for (unsigned i = 0; i < 15; ++i)
    std::fprintf(stderr, " r%u=%08x", i, static_cast<std::uint32_t>(cpu.registers[i]));
  std::fprintf(stderr, "\n");
}

void load_state(arm64::CpuState &cpu, const ref::State &s) {
  for (unsigned i = 0; i < 15; ++i)
    cpu.registers[i] = s.r[i];
  cpu.nzcv = s.nzcv();
}

ref::State random_state(Random &random) {
  ref::State s;
  for (unsigned i = 0; i < 15; ++i)
    s.r[i] = random.value();
  s.set_nzcv(static_cast<std::uint32_t>(random.next()));
  return s;
}

Instruction random_a32_dp(Random &random) {
  const unsigned cond = random.below(8) == 0 ? random.below(15) : 14;
  const auto op = static_cast<ref::DpOp>(random.below(16));
  const bool tests = ref::dp_tests_only(op);
  const bool s = tests || random.below(2) != 0;
  const unsigned rd = tests ? 0 : random.below(15);
  const bool moves = op == ref::DpOp::mov || op == ref::DpOp::mvn;
  const unsigned rn = moves ? 0 : random.below(15);
  switch (random.below(3)) {
  case 0:
    return {a32_dp_imm(cond, op, s, rd, rn, random.below(4096)), false};
  case 1:
    return {a32_dp_reg(cond, op, s, rd, rn, random.below(15), random.below(4),
                       random.below(32)),
            false};
  default:
    return {a32_dp_rsr(cond, op, s, rd, rn, random.below(15), random.below(4),
                       random.below(15)),
            false};
  }
}

/* Runs `words` followed by an SVC at `offset` from `initial`, and returns the
 * task after it. */
arm64::CpuState run_sequence(Fixture &fixture, std::size_t offset,
                             const std::vector<std::uint32_t> &words,
                             const ref::State &initial) {
  for (std::size_t i = 0; i < words.size(); ++i)
    fixture.code32(offset + 4 * i, {words[i]});
  fixture.code32(offset + 4 * words.size(), {0xef000000U}); // svc #0
  auto cpu = Fixture::task(false, offset);
  load_state(cpu, initial);
  const auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == words.size() + 1U);
  require(cpu.pc == key::a32_key(code_base + static_cast<std::uint32_t>(
                                                 offset + 4 * (words.size() + 1U))));
  return cpu;
}

ref::State reference_run(const std::vector<std::uint32_t> &words,
                         const ref::State &initial) {
  ref::State expected = initial;
  for (const auto word : words)
    a32_reference_step(expected, word);
  return expected;
}

/* The first prefix of a failing sequence that still fails, with the state
 * before its last instruction. */
[[noreturn]] void bisect(Fixture &fixture, std::size_t scratch,
                         const std::vector<std::uint32_t> &words,
                         const ref::State &initial) {
  for (std::size_t k = 1; k <= words.size(); ++k) {
    const std::vector<std::uint32_t> prefix(words.begin(), words.begin() + k);
    const auto expected = reference_run(prefix, initial);
    const auto cpu = run_sequence(fixture, scratch + k * 1024, prefix, initial);
    if (!state_matches(cpu, expected)) {
      std::fprintf(stderr, "first failing instruction: #%zu = %08x\n", k - 1,
                   words[k - 1]);
      report_state(cpu, expected, prefix, k, initial, "prefix");
      break;
    }
  }
  std::abort();
}

void a32_random_sequences(unsigned long generation, std::uint64_t seed,
                          unsigned programs, unsigned length) {
  constexpr std::size_t stride = 1024;
  const std::size_t program_bytes = static_cast<std::size_t>(programs) * stride;
  const std::size_t scratch = (program_bytes + guest_page - 1) / guest_page * guest_page;
  const std::size_t pages = scratch / guest_page + (length + 2) * stride / guest_page + 1;
  Fixture fixture{generation, pages};
  Random random{seed};
  for (unsigned p = 0; p < programs; ++p) {
    std::vector<std::uint32_t> program;
    for (unsigned i = 0; i < length; ++i)
      program.push_back(random_a32_dp(random).word);
    const auto initial = random_state(random);
    const auto expected = reference_run(program, initial);
    const auto cpu = run_sequence(fixture, static_cast<std::size_t>(p) * stride,
                                  program, initial);
    if (!state_matches(cpu, expected)) {
      report_state(cpu, expected, program, length, initial, "a32 random");
      bisect(fixture, scratch, program, initial);
    }
  }
}

// ---------------------------------------------------------------------------
// Parallel add and subtract: every form against a model of the lane
// arithmetic written for this test. The forms but the signed saturating ones
// run in the vector unit, so they must not reach the library, and none of
// them may change the registers they do not name or (but for the ones that
// set GE) the GE flags.

struct ParallelResult final {
  std::uint32_t value;
  std::uint32_t ge;
};

ParallelResult parallel_model(unsigned prefix, unsigned op2, std::uint32_t a,
                              std::uint32_t b, std::uint32_t old_ge) {
  // prefix: 1 S, 2 Q, 3 SH, 5 U, 6 UQ, 7 UH. op2: 0 ADD16, 1 ASX, 2 SAX,
  // 3 SUB16, 4 ADD8, 7 SUB8.
  const bool is_signed = prefix <= 3U;
  const unsigned width = op2 == 4U || op2 == 7U ? 8U : 16U;
  const unsigned lanes = 32U / width;
  const std::int64_t limit = std::int64_t{1} << width;
  const auto element = [&](std::uint32_t value, unsigned lane) {
    const std::int64_t raw = value >> (width * lane) & (limit - 1);
    return is_signed && raw >= limit / 2 ? raw - limit : raw;
  };
  std::uint32_t value = 0;
  std::uint32_t ge = 0;
  for (unsigned lane = 0; lane < lanes; ++lane) {
    bool subtract = op2 == 3U || op2 == 7U;
    unsigned b_lane = lane;
    if (op2 == 1U) {
      subtract = lane == 0U;
      b_lane = 1U - lane;
    } else if (op2 == 2U) {
      subtract = lane == 1U;
      b_lane = 1U - lane;
    }
    const std::int64_t x = element(a, lane);
    const std::int64_t y = element(b, b_lane);
    const std::int64_t exact = subtract ? x - y : x + y;
    std::int64_t out = exact;
    bool lane_ge = false;
    switch (prefix) {
    case 1: lane_ge = exact >= 0; break;
    case 5: lane_ge = subtract ? exact >= 0 : exact >= limit; break;
    case 2:
      out = std::max(-limit / 2, std::min(limit / 2 - 1, exact));
      break;
    case 6: out = std::max<std::int64_t>(0, std::min(limit - 1, exact)); break;
    default: out = exact >> 1; break;
    }
    value |= static_cast<std::uint32_t>(out & (limit - 1)) << (width * lane);
    if (lane_ge)
      ge |= (width == 8U ? 0xffU : 0xffffU) << (width * lane);
  }
  const bool sets_ge = prefix == 1U || prefix == 5U;
  return {value, sets_ge ? ge : old_ge};
}

void parallel_arithmetic() {
  Fixture fixture{0x1a32'0301, 4};
  Random random{0x7a11'e1};
  constexpr std::uint32_t special[] = {
      0x0000'0000U, 0xffff'ffffU, 0x8080'8080U, 0x7f7f'7f7fU, 0x0001'0001U,
      0x8000'8000U, 0x7fff'7fffU, 0x00ff'00ffU, 0xff00'ff00U, 0x0100'0100U,
      0x807f'7f80U, 0x7f80'807fU, 0xfffe'0002U, 0x0102'0304U, 0xfffd'fcfbU};
  struct Registers final {
    unsigned d, n, m;
  };
  constexpr Registers shapes[] = {{2, 3, 4}, {3, 3, 4}, {4, 3, 4},
                                  {3, 3, 3}, {12, 5, 6}, {0, 1, 2}};
  aarch32::reset_a32_library_exits();
  std::size_t executed_inline = 0;
  for (const unsigned prefix : {1U, 2U, 3U, 5U, 6U, 7U}) {
    for (const unsigned op2 : {0U, 1U, 2U, 3U, 4U, 7U}) {
      for (const auto &shape : shapes) {
        const std::uint32_t word = 0xe600'0f10U | prefix << 20 | shape.n << 16 |
                                   shape.d << 12 | op2 << 5 | shape.m;
        fixture.code32(0, {word, 0xef00'0000U});
        for (unsigned round = 0; round < 80; ++round) {
          std::uint32_t a = random.value();
          std::uint32_t b = random.value();
          if (round < std::size(special) * 2U) {
            a = special[round % std::size(special)];
            b = special[(round / 2U + 3U) % std::size(special)];
          }
          const std::uint32_t old_ge = static_cast<std::uint32_t>(random.next()) &
                                       0x0101'0101U * 0xffU;
          auto cpu = Fixture::task(false, 0);
          for (unsigned i = 0; i < 15; ++i)
            cpu.registers[i] = 0x1000'0000U + i;
          cpu.registers[shape.n] = a;
          cpu.registers[shape.m] = shape.m == shape.n ? a : b;
          cpu.a32_ge_mask = old_ge;
          const auto before = cpu;
          const bool counted = prefix != 2U;
          aarch32::count_a32_library_exits(counted);
          const auto outcome = fixture.run(cpu);
          aarch32::count_a32_library_exits(false);
          require(outcome.reason == ExitReason::system_call);
          const std::uint32_t right = shape.m == shape.n ? a : b;
          const auto expected = parallel_model(prefix, op2, a, right, old_ge);
          const auto got = static_cast<std::uint32_t>(cpu.registers[shape.d]);
          if (got != expected.value || cpu.a32_ge_mask != expected.ge) {
            std::fprintf(stderr,
                         "parallel %08x a=%08x b=%08x: got %08x ge %08x, "
                         "expected %08x ge %08x\n",
                         word, a, right, got, cpu.a32_ge_mask, expected.value,
                         expected.ge);
            std::abort();
          }
          for (unsigned i = 0; i < 15; ++i)
            if (i != shape.d)
              require(cpu.registers[i] == before.registers[i]);
          executed_inline += counted ? 1U : 0U;
        }
      }
    }
  }
  require(executed_inline > 0U);
  require(aarch32::a32_library_exit_total() == 0U);
}

} // namespace

int main(int argc, char **argv) {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  unsigned programs = 600;
  if (argc > 1)
    programs = static_cast<unsigned>(std::atoi(argv[1]));
  a32_random_sequences(0x1a32'0100, 0x1234'5678'9abcULL, programs, 24);
  parallel_arithmetic();
  std::puts("arm64-jit-a32-data-processing: ok");
  return 0;
}
