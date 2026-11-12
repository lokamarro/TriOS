// SPDX-License-Identifier: GPL-2.0-or-later
/* The transfer instructions on the JIT against a reference written from the
 * encodings: random sequences of loads and stores of every size and
 * addressing mode, LDRD/STRD and LDM/STM, over addresses that are aligned,
 * unaligned, across a page, at the end of the mapping and unmapped. Faults
 * are checked too: the retired count, the address and kind, and that the
 * faulting instruction changed nothing. */
#include "arm64_jit_a32_reference.hpp"
#include "arm64_jit_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/aarch32/semantics_exit_counters.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

using namespace uml::emulator::tests;
using namespace uml::emulator::tests::a32;
using ::uml::cpu::ExitReason;

namespace {

namespace aarch32 = ::uml::cpu::arm64::aarch32;

constexpr std::size_t data_pages = 2;
constexpr std::size_t data_bytes = data_pages * guest_page;

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
  bool chance(unsigned percent) { return below(100) < percent; }

private:
  std::uint64_t state_;
};

/* The end of the code mapping. An access there is not a fault on the JIT's
 * guest, and a store would rewrite the program, so the case is discarded. */
std::uint32_t code_end;

/* The mapped data region. */
struct Memory final {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(data_bytes);
  mutable bool foreign{};

  [[nodiscard]] static bool mapped(std::uint32_t address) {
    return address >= data_base && address - data_base < data_bytes;
  }
  /* False with the first unmapped chunk's address in `fault`. */
  [[nodiscard]] bool touch(std::uint32_t address, std::size_t size,
                           std::uint32_t &fault) const {
    std::uint32_t cursor = address;
    std::size_t remaining = size;
    while (remaining != 0) {
      if (!mapped(cursor)) {
        fault = cursor;
        foreign |= cursor >= code_base && cursor < code_end;
        return false;
      }
      const std::size_t chunk =
          std::min<std::size_t>(remaining, guest_page - (cursor & (guest_page - 1)));
      cursor += static_cast<std::uint32_t>(chunk);
      remaining -= chunk;
    }
    return true;
  }
  [[nodiscard]] std::uint64_t load(std::uint32_t address, unsigned size) const {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i)
      value |= std::uint64_t{bytes[address + i - data_base]} << (8 * i);
    return value;
  }
  void store(std::uint32_t address, unsigned size, std::uint64_t value) {
    for (unsigned i = 0; i < size; ++i)
      bytes[address + i - data_base] = static_cast<std::uint8_t>(value >> (8 * i));
  }
};

struct Fault final {
  bool happened{};
  std::uint32_t address{};
  bool write{};
  bool alignment{};
};

/* One A32 transfer on the reference. Returns the fault, if any. */
Fault step(ref::State &s, Memory &memory, std::uint32_t word) {
  const unsigned cond = word >> 28;
  if (!ref::condition_passed(s, cond))
    return {};
  const unsigned op = word >> 25 & 7U;
  const unsigned rn = word >> 16 & 15U;
  const unsigned rt = word >> 12 & 15U;
  const bool p = (word >> 24 & 1U) != 0U;
  const bool u = (word >> 23 & 1U) != 0U;
  const bool w = (word >> 21 & 1U) != 0U;
  const bool l = (word >> 20 & 1U) != 0U;
  std::uint32_t fault = 0;
  const auto fail = [&](bool write, bool alignment = false) {
    return Fault{true, fault, write, alignment};
  };

  if (op == 4) { // LDM/STM
    const unsigned list = word & 0xffffU;
    const unsigned count = std::popcount(list);
    const std::uint32_t base = s.r[rn];
    std::uint32_t start;
    std::uint32_t final_base;
    const bool increment = u;
    const bool before = p;
    if (increment) {
      start = base + (before ? 4U : 0U);
      final_base = base + 4U * count;
    } else {
      start = base - 4U * count + (before ? 0U : 4U);
      final_base = base - 4U * count;
    }
    if ((start & 3U) != 0U) {
      fault = start;
      return fail(!l, true);
    }
    if (!memory.touch(start, 4U * count, fault))
      return fail(!l);
    std::uint32_t address = start;
    for (unsigned r = 0; r < 16U; ++r) {
      if ((list >> r & 1U) == 0U)
        continue;
      if (l)
        s.r[r] = static_cast<std::uint32_t>(memory.load(address, 4));
      else
        memory.store(address, 4, s.r[r]);
      address += 4;
    }
    if (w)
      s.r[rn] = final_base;
    return {};
  }

  const bool extra = op == 0 && (word >> 7 & 1U) != 0U && (word >> 4 & 1U) != 0U;
  const bool reg_form = extra ? (word >> 22 & 1U) == 0U : (word >> 25 & 1U) != 0U;
  std::uint32_t offset;
  if (extra) {
    offset = reg_form ? s.r[word & 15U] : ((word >> 4 & 0xf0U) | (word & 15U));
  } else if (reg_form) {
    unsigned amount = word >> 7 & 31U;
    auto type = static_cast<ref::Shift>(word >> 5 & 3U);
    if ((type == ref::Shift::lsr || type == ref::Shift::asr) && amount == 0)
      amount = 32;
    if (type == ref::Shift::ror && amount == 0)
      type = ref::Shift::rrx;
    offset = ref::shift_c(s.r[word & 15U], type, amount, s.c).value;
  } else {
    offset = word & 0xfffU;
  }
  const std::uint32_t base = s.r[rn];
  const std::uint32_t displaced = u ? base + offset : base - offset;
  const std::uint32_t address = p ? displaced : base;
  const bool wback = !p || w;

  unsigned size = 4;
  bool sign = false;
  bool dual = false;
  if (extra) {
    const unsigned sh = word >> 5 & 3U;
    if (l) {
      size = 2;
      sign = sh == 2 || sh == 3;
      if (sh == 2) size = 1;
    } else if (sh == 1) {
      size = 2;
    } else {
      dual = true;
      // sh == 2 is LDRD (L=0), sh == 3 is STRD.
      size = 8;
    }
  } else if ((word >> 22 & 1U) != 0U) {
    size = 1;
  }

  if (dual) {
    const bool load_dual = (word >> 5 & 3U) == 2U;
    if ((address & 3U) != 0U) {
      fault = address;
      return fail(!load_dual, true);
    }
    if (!memory.touch(address, 8, fault))
      return fail(!load_dual);
    if (load_dual) {
      const auto value = memory.load(address, 8);
      if (wback)
        s.r[rn] = displaced;
      s.r[rt] = static_cast<std::uint32_t>(value);
      s.r[rt + 1] = static_cast<std::uint32_t>(value >> 32);
    } else {
      memory.store(address, 8, std::uint64_t{s.r[rt]} | std::uint64_t{s.r[rt + 1]} << 32);
      if (wback)
        s.r[rn] = displaced;
    }
    return {};
  }

  if (!memory.touch(address, size, fault))
    return fail(!l);
  if (l) {
    auto value = static_cast<std::uint32_t>(memory.load(address, size));
    if (sign)
      value = size == 1 ? static_cast<std::uint32_t>(static_cast<std::int8_t>(value))
                        : static_cast<std::uint32_t>(static_cast<std::int16_t>(value));
    if (wback)
      s.r[rn] = displaced;
    s.r[rt] = value;
  } else {
    memory.store(address, size, s.r[rt]);
    if (wback)
      s.r[rn] = displaced;
  }
  return {};
}

// --- Generation ------------------------------------------------------------

std::uint32_t pointer(Random &random) {
  const std::uint32_t end = data_base + data_bytes;
  switch (random.below(10)) {
  case 0: return end - random.below(32);                       // end of the mapping
  case 1: return data_base + guest_page - 16U + random.below(32); // across the page
  case 2: return end + random.below(64);                       // unmapped
  case 3: return data_base - random.below(64);                 // unmapped below
  case 4: return random.below(64);                             // low
  default: return data_base + random.below(static_cast<unsigned>(data_bytes));
  }
}

/* A clean program keeps its addresses mapped: r0-r7 hold small offsets, r8-r14
 * hold pointers, and the transfers use them that way, so most of a sequence
 * retires. A chaotic program uses any register for anything and faults early. */
struct Style final {
  bool clean{};
};

unsigned reg(Random &random) { return random.below(15); }
unsigned base_reg(Random &random, Style style) {
  return style.clean ? 8U + random.below(7) : reg(random);
}
unsigned offset_reg(Random &random, Style style) {
  return style.clean ? random.below(8) : reg(random);
}
unsigned data_reg(Random &random, Style style) {
  return style.clean && random.chance(96) ? random.below(8) : reg(random);
}

std::uint32_t random_transfer(Random &random, Style style) {
  const unsigned cond = random.chance(12) ? random.below(14) : 14U;
  const unsigned kind = random.below(10);
  const unsigned rn = base_reg(random, style);
  unsigned rt = data_reg(random, style);
  const bool p = random.chance(75);
  const bool w = p && random.chance(40);
  const bool wback = !p || w;
  const bool u = random.chance(70);
  const bool load = random.chance(50);
  const auto small_imm = [&](unsigned bound) {
    const unsigned value = random.chance(70) ? random.below(33) : random.below(bound);
    return style.clean && random.chance(85) ? value & ~3U : value;
  };
  if (kind < 4) { // LDR/STR/LDRB/STRB, immediate or register
    const bool byte = random.chance(40);
    const bool reg_form = random.chance(35);
    if (wback && rt == rn)
      rt = (rt + 1) % 15;
    std::uint32_t word = cond << 28 | 1U << 26 | (reg_form ? 1U << 25 : 0U) |
                         (p ? 1U << 24 : 0U) | (u ? 1U << 23 : 0U) |
                         (byte ? 1U << 22 : 0U) | (w ? 1U << 21 : 0U) |
                         (load ? 1U << 20 : 0U) | rn << 16 | rt << 12;
    if (reg_form) {
      const unsigned amount = style.clean ? random.below(3) : random.below(32);
      const unsigned type = style.clean ? 0U : random.below(4);
      word |= amount << 7 | type << 5 | offset_reg(random, style);
    } else {
      word |= small_imm(style.clean ? 256 : 4096);
    }
    return word;
  }
  if (kind < 7) { // halfword and signed, immediate or register
    const bool reg_form = random.chance(40);
    unsigned code = random.below(4); // 0 STRH, 1 LDRH, 2 LDRSB, 3 LDRSH
    if (wback && rt == rn)
      rt = (rt + 1) % 15;
    unsigned l = code == 0 ? 0 : 1;
    unsigned sh = code == 0 ? 1 : code == 1 ? 1 : code == 2 ? 2 : 3;
    std::uint32_t word = cond << 28 | (p ? 1U << 24 : 0U) | (u ? 1U << 23 : 0U) |
                         (reg_form ? 0U : 1U << 22) | (w ? 1U << 21 : 0U) |
                         l << 20 | rn << 16 | rt << 12 | 1U << 7 | sh << 5 | 1U << 4;
    if (reg_form) {
      word |= offset_reg(random, style);
    } else {
      const unsigned imm = small_imm(256);
      word |= (imm >> 4) << 8 | (imm & 15U);
    }
    return word;
  }
  if (kind < 8) { // LDRD/STRD
    const unsigned t = style.clean ? random.below(4) * 2U : random.below(7) * 2U;
    const bool reg_form = random.chance(40);
    unsigned base = rn;
    if (wback && (base == t || base == t + 1))
      base = (t + 2U) % 15U;
    const unsigned sh = load ? 2U : 3U;
    // LDRD is L=0 S=1 H=0, STRD is L=0 S=1 H=1.
    std::uint32_t word = cond << 28 | (p ? 1U << 24 : 0U) | (u ? 1U << 23 : 0U) |
                         (reg_form ? 0U : 1U << 22) | (w ? 1U << 21 : 0U) |
                         base << 16 | t << 12 | 1U << 7 | sh << 5 | 1U << 4;
    if (reg_form) {
      unsigned m = offset_reg(random, style);
      if (m == t || m == t + 1U)
        m = (t + 2U) % 15U; // Rm may not be either transfer register
      word |= m;
    } else {
      const unsigned imm = small_imm(256);
      word |= (imm >> 4) << 8 | (imm & 15U);
    }
    return word;
  }
  // LDM/STM
  unsigned list = 0;
  while (list == 0)
    list = static_cast<unsigned>(random.next()) &
           (style.clean ? 0xffU : random.chance(50) ? 0x1fU : 0x1fffU);
  list &= 0x7fffU; // no PC
  if (list == 0)
    list = 1;
  const bool wb = random.chance(50);
  unsigned base = rn;
  if (wb) {
    // LDM with the base in the list is unpredictable; STM only when the base
    // is the lowest.
    if ((list >> base & 1U) != 0U && (load || std::countr_zero(list) != static_cast<int>(base)))
      list &= ~(1U << base);
    if (list == 0)
      list = 1U << ((base + 1) % 15);
  }
  const bool up = random.chance(50);
  const bool before = random.chance(50);
  return cond << 28 | 4U << 25 | (before ? 1U << 24 : 0U) | (up ? 1U << 23 : 0U) |
         (wb ? 1U << 21 : 0U) | (load ? 1U << 20 : 0U) | base << 16 | list;
}

std::uint32_t clean_pointer(Random &random) {
  std::uint32_t address;
  if (random.chance(30))
    address = data_base + guest_page - 48U + random.below(96);
  else
    address = data_base + 512U + random.below(static_cast<unsigned>(data_bytes) - 1024U);
  return random.chance(88) ? address & ~3U : address;
}

ref::State random_state(Random &random, Style style) {
  ref::State s;
  if (style.clean) {
    for (unsigned i = 0; i < 8; ++i)
      s.r[i] = random.chance(70) ? random.below(16) * 4U : random.below(64);
    for (unsigned i = 8; i < 15; ++i)
      s.r[i] = clean_pointer(random);
    s.set_nzcv(static_cast<std::uint32_t>(random.next()));
    return s;
  }
  for (unsigned i = 0; i < 15; ++i)
    s.r[i] = random.chance(75) ? pointer(random) : static_cast<std::uint32_t>(random.next());
  s.set_nzcv(static_cast<std::uint32_t>(random.next()));
  return s;
}

struct Case final {
  std::vector<std::uint32_t> program;
  ref::State initial;
  std::vector<std::uint8_t> memory;
};

struct Expectation final {
  ref::State state;
  Memory memory;
  Fault fault;
  std::size_t retired{};
  bool foreign{};
};

Expectation reference(const Case &c) {
  Expectation e;
  e.state = c.initial;
  e.memory.bytes = c.memory;
  for (const auto word : c.program) {
    const auto fault = step(e.state, e.memory, word);
    if (fault.happened) {
      e.fault = fault;
      e.foreign = e.memory.foreign;
      return e;
    }
    ++e.retired;
  }
  return e;
}

void put_memory(Fixture &fixture, const std::vector<std::uint8_t> &bytes) {
  fixture.data_bytes(0, std::as_bytes(std::span{bytes}));
}

/* Runs the case; returns the mismatch description or nothing. */
unsigned faulting_cases;
unsigned alignment_faults;
unsigned clean_programs;
unsigned clean_faults;
bool current_clean;
unsigned retired_instructions;

std::optional<std::string> run_case(Fixture &fixture, std::size_t offset,
                                    const Case &c) {
  const auto e = reference(c);
  if (e.foreign)
    return std::nullopt;
  for (std::size_t i = 0; i < c.program.size(); ++i)
    fixture.code32(offset + 4 * i, {c.program[i]});
  fixture.code32(offset + 4 * c.program.size(), {0xef000000U});
  put_memory(fixture, c.memory);
  auto cpu = Fixture::task(false, offset);
  for (unsigned i = 0; i < 15; ++i)
    cpu.registers[i] = c.initial.r[i];
  cpu.nzcv = c.initial.nzcv();
  const auto result = fixture.run(cpu);
  char text[256];
  const auto bad = [&](const char *what) {
    std::snprintf(text, sizeof text, "%s (retired %zu, expected %zu)", what,
                  result.retired_instructions, e.retired);
    return std::string{text};
  };
  retired_instructions += static_cast<unsigned>(e.retired);
  if (e.fault.happened) {
    ++faulting_cases;
    alignment_faults += e.fault.alignment ? 1U : 0U;
    clean_faults += current_clean ? 1U : 0U;
    if (current_clean && std::getenv("A32_MEM_TRACE") != nullptr)
      std::fprintf(stderr, "clean fault at #%zu %08x addr %08x align=%d write=%d\n",
                   e.retired, c.program[e.retired], e.fault.address,
                   e.fault.alignment, e.fault.write);
    if (result.reason != ExitReason::memory_fault) {
      std::snprintf(text, sizeof text,
                    "expected a memory fault at %08x, got reason %d (retired %zu, "
                    "expected %zu)",
                    e.fault.address, static_cast<int>(result.reason),
                    result.retired_instructions, e.retired);
      return std::string{text};
    }
    if (result.retired_instructions != e.retired)
      return bad("retired count");
    if (result.fault.address != e.fault.address)
      return bad("fault address");
    if ((result.fault.access == ::uml::memory::MemoryAccess::write) != e.fault.write)
      return bad("fault access");
    if (result.fault.alignment != e.fault.alignment)
      return bad("fault alignment");
    if (cpu.pc != key::a32_key(code_base + static_cast<std::uint32_t>(offset + 4 * e.retired)))
      return bad("fault pc");
  } else {
    if (result.reason != ExitReason::system_call)
      return bad("expected the svc");
    if (result.retired_instructions != c.program.size() + 1)
      return bad("retired count");
  }
  for (unsigned i = 0; i < 15; ++i)
    if (static_cast<std::uint32_t>(cpu.registers[i]) != e.state.r[i] ||
        (cpu.registers[i] >> 32) != 0)
      return bad("register");
  const auto actual = fixture.read_data(0, data_bytes);
  if (std::memcmp(actual.data(), e.memory.bytes.data(), data_bytes) != 0)
    return bad("memory");
  return std::nullopt;
}

[[noreturn]] void fail(Fixture &fixture, std::size_t scratch, const Case &c,
                       const std::string &what) {
  std::fprintf(stderr, "MISMATCH: %s\n  program:", what.c_str());
  for (const auto word : c.program)
    std::fprintf(stderr, " %08x", word);
  std::fprintf(stderr, "\n  initial nzcv=%08x:", c.initial.nzcv());
  for (unsigned i = 0; i < 15; ++i)
    std::fprintf(stderr, " r%u=%08x", i, c.initial.r[i]);
  std::fprintf(stderr, "\n");
  // The shortest failing prefix.
  for (std::size_t k = 1; k <= c.program.size(); ++k) {
    Case prefix = c;
    prefix.program.assign(c.program.begin(), c.program.begin() + k);
    if (const auto m = run_case(fixture, scratch + k * 1024, prefix)) {
      std::fprintf(stderr, "first failing instruction: #%zu = %08x (%s)\n", k - 1,
                   c.program[k - 1], m->c_str());
      break;
    }
  }
  std::abort();
}

void random_sequences(unsigned long generation, std::uint64_t seed,
                      unsigned programs, unsigned length) {
  constexpr std::size_t stride = 1024;
  const std::size_t program_bytes = static_cast<std::size_t>(programs) * stride;
  const std::size_t scratch = (program_bytes + guest_page - 1) / guest_page * guest_page;
  const std::size_t code_pages = scratch / guest_page + (length + 2) * stride / guest_page + 1;
  Fixture fixture{generation, code_pages, data_pages};
  code_end = code_base + static_cast<std::uint32_t>(code_pages * guest_page);
  Random random{seed};
  for (unsigned p = 0; p < programs; ++p) {
    Case c;
    const Style style{.clean = (p & 1U) == 0U};
    current_clean = style.clean;
    clean_programs += style.clean ? 1U : 0U;
    for (unsigned i = 0; i < length; ++i)
      c.program.push_back(random_transfer(random, style));
    c.initial = random_state(random, style);
    c.memory.resize(data_bytes);
    for (std::size_t i = 0; i < data_bytes; ++i)
      c.memory[i] = static_cast<std::uint8_t>(random.next());
    if (style.clean) {
      /* Words that are small offsets, so a loaded register stays usable. */
      for (std::size_t i = 0; i < data_bytes; i += 4)
        if (random.chance(92)) {
          const unsigned value = random.chance(50) ? 0U : random.below(0x30) & ~3U;
          c.memory[i] = static_cast<std::uint8_t>(value);
          c.memory[i + 1] = c.memory[i + 2] = c.memory[i + 3] = 0;
        }
    }
    if (const auto mismatch = run_case(fixture, static_cast<std::size_t>(p) * stride, c))
      fail(fixture, scratch, c, *mismatch);
  }
}

/* A load whose destination is the offset register of a writeback form
 * recomputes the base from the offset the instruction read, not the value it
 * loaded: for every size, shift, direction and index form, alone and in
 * sequences, the reference agrees and no instruction takes the library. */
void loads_over_the_offset() {
  constexpr std::size_t stride = 64;
  constexpr std::uint32_t all = 0xe000'0000U;
  constexpr std::uint32_t z_flag = 0x4000'0000U;
  constexpr std::uint32_t c_flag = 0x2000'0000U;
  constexpr std::uint32_t pre_up = 0x07b1'2002U;   // ldr r2, [r1, r2]!
  constexpr std::uint32_t pre_down = 0x0731'2002U; // ldr r2, [r1, -r2]!
  constexpr std::uint32_t post_up = 0x0691'2002U;  // ldr r2, [r1], r2
  constexpr std::uint32_t byte = 1U << 22;
  constexpr std::uint32_t shift_lsl2 = 0x2U << 7;
  constexpr std::uint32_t shift_lsr4 = 0x4U << 7 | 1U << 5;
  constexpr std::uint32_t shift_asr1 = 0x1U << 7 | 2U << 5;
  constexpr std::uint32_t shift_ror3 = 0x3U << 7 | 3U << 5;
  constexpr std::uint32_t shift_rrx = 3U << 5;
  struct Program final {
    std::vector<std::uint32_t> words;
    bool carry_matters;
  };
  const std::vector<Program> programs = {
      {{all | pre_up}, false},
      {{all | pre_up | byte}, false},
      {{all | post_up}, false},
      {{all | post_up | byte}, false},
      {{all | pre_down}, false},
      {{all | pre_down | byte}, false},
      {{all | pre_up | shift_lsl2}, false},
      {{all | pre_up | shift_lsr4}, false},
      {{all | pre_up | shift_asr1}, false},
      {{all | pre_up | shift_ror3}, false},
      {{all | pre_up | shift_rrx}, true},
      {{0x07b1'3003U | all}, false},            // ldr r3, [r1, r3]!
      {{0x07b6'4004U | all}, false},            // ldr r4, [r6, r4]!
      {{0xe1b1'20b2U}, false},                  // ldrh r2, [r1, r2]!
      {{0xe1b1'20d2U}, false},                  // ldrsb r2, [r1, r2]!
      {{0xe1b1'20f2U}, false},                  // ldrsh r2, [r1, r2]!
      {{0xe091'20b2U}, false},                  // ldrh r2, [r1], r2
      {{0x17b1'2002U}, false},                  // ldrne r2, [r1, r2]!
      {{0x07b1'2002U}, false},                  // ldreq r2, [r1, r2]!
      {{0x2791'2002U}, false},                  // ldrcs r2, [r1, r2]
      {{all | pre_up, all | pre_up, 0xe7b1'3003U, 0xe691'4004U | byte,
        0xe7b1'2082U, 0xe1b1'30b3U, 0xe7b1'5005U, 0xe791'6006U},
       false},
  };
  constexpr std::size_t code_pages = 8;
  Fixture fixture{0x1a32'0201, code_pages, data_pages};
  code_end = code_base + static_cast<std::uint32_t>(code_pages * guest_page);
  aarch32::reset_a32_library_exits();
  aarch32::count_a32_library_exits(true);
  std::size_t offset = 0;
  for (const auto &program : programs) {
    for (const std::uint32_t flags : {std::uint32_t{0}, z_flag, c_flag}) {
      if (program.carry_matters && flags == c_flag)
        continue;
      for (const std::uint32_t distance : {0x10U, 0x08U, 0x24U}) {
        Case c;
        c.program = program.words;
        for (unsigned i = 0; i < 15; ++i)
          c.initial.r[i] = i < 8 ? 4U * i : data_base + 0x40U * i;
        c.initial.r[1] = data_base + 0x100;
        c.initial.r[6] = data_base + 0x180;
        c.initial.r[2] = distance;
        c.initial.r[3] = distance;
        c.initial.r[4] = distance;
        c.initial.r[5] = distance;
        c.initial.set_nzcv(flags);
        c.memory.resize(data_bytes);
        for (std::size_t i = 0; i < data_bytes; i += 4) {
          c.memory[i] = static_cast<std::uint8_t>((i / 4 * 4) % 0x30);
          c.memory[i + 1] = c.memory[i + 2] = c.memory[i + 3] = 0;
        }
        if (const auto mismatch = run_case(fixture, offset, c))
          fail(fixture, 0, c, *mismatch);
        offset += stride;
        require(offset + stride <= code_pages * guest_page);
      }
    }
  }
  require(aarch32::a32_library_exit_total() == 0U);
  aarch32::count_a32_library_exits(false);
}

} // namespace

int main(int argc, char **argv) {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  unsigned programs = 600;
  if (argc > 1)
    programs = static_cast<unsigned>(std::atoi(argv[1]));
  random_sequences(0x1a32'0200, 0x9e37'79b9'7f4aULL, programs, 10);
  loads_over_the_offset();
  std::printf("arm64-jit-a32-memory: ok (%u programs, %u faulting, %u alignment, "
              "%u clean, %u clean faulting, %u instructions)\n",
              programs, faulting_cases, alignment_faults, clean_programs,
              clean_faults, retired_instructions);
  return 0;
}
