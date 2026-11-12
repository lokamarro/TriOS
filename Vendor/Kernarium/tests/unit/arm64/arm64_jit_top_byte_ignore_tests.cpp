// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A JIT translation is compiled for top-byte ignore (TBI) and its key says so.
 * With it, every access reaches the byte its low 56 bits name, whatever
 * register is its base, a fault reports that untagged address, and the base
 * register keeps its tag. Without it, the tag is part of the address.
 *
 * Each family of memory form runs one program on one executor: untagged
 * without TBI, which is the reference; tagged with TBI, which must leave the
 * same registers, vectors and memory with the base keeping its tag; tagged
 * without TBI, which must fault at the tagged address before anything moves;
 * tagged with TBI again, which finds the stripped translation once more; and
 * tagged at an unmapped page, with and without TBI. Running them on one
 * executor shows that the key, not a stale translation, selects the strip.
 */
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "platform/apple/host_features.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_runtime_contract.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace uml::emulator::tests;

namespace {

namespace arm64 = uml::cpu::arm64;
using uml::cpu::arm64::JitExecutor;
using uml::memory::StaticGuestMemory;
using Event = ::uml::cpu::EngineEvent;

constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr std::uint64_t base = 0x12c'0000;
/* Two data pages, read-write, and a third page nothing maps. */
constexpr std::uint64_t data = base + page;
constexpr std::uint64_t unmapped = base + 16U * page;
constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::uint64_t low_bits = (std::uint64_t{1} << 56) - 1U;
constexpr std::array<std::uint64_t, 3> tags{
    std::uint64_t{0x5a} << 56, std::uint64_t{0xff} << 56,
    std::uint64_t{0x01} << 56};

struct Family final {
  const char *name;
  std::vector<std::uint32_t> program;
  /* The first byte the program accesses, as an offset from x0 (or SP). */
  std::uint64_t first_access;
  /* The bytes the program accesses from there; a family with a span of 8 or
   * more also runs across the end of the first data page. */
  std::uint64_t span;
  /* x2, the index of a register-offset form. */
  std::uint64_t index{1};
  /* The alignment of the first byte of the access an instruction reports. */
  std::uint64_t granule{1};
  bool lse{};
  /* The program retries a store-exclusive that fails, which hardware may do
   * for no reason of the program's, so how many instructions it retires is
   * not fixed. */
  bool retries{};
};

const std::vector<Family> &families() {
  static const std::vector<Family> result{
      {"str unsigned", {0xf900'0401U}, 8, 8},         // str x1,[x0,#8]
      {"ldr unsigned", {0xf940'0401U}, 8, 8},         // ldr x1,[x0,#8]
      {"ldr register offset", {0xf862'6801U}, 8, 8, 8},   // ldr x1,[x0,x2]
      {"str register offset scaled", {0xf822'7801U}, 8, 8},
                                                      // str x1,[x0,x2,lsl #3]
      {"ldur", {0xf85f'8001U}, static_cast<std::uint64_t>(-8), 8},
      {"ldr pre-index", {0xf840'8c01U}, 8, 8},        // ldr x1,[x0,#8]!
      {"ldr post-index", {0xf840'8401U}, 0, 8},       // ldr x1,[x0],#8
      {"ldp pre-index", {0xa9c1'0801U}, 16, 16},      // ldp x1,x2,[x0,#16]!
      {"stp post-index", {0xa881'0801U}, 0, 16},      // stp x1,x2,[x0],#16
      {"ld1 post-index", {0x4cdf'7000U}, 0, 16},      // ld1 {v0.16b},[x0],#16
      {"gpr load run", {0xf940'0803U, 0xf940'0c04U, 0xf940'1005U}, 16, 24},
      {"gpr store run", {0xf900'0803U, 0xf900'0c04U, 0xf900'1005U}, 16, 24},
      {"ldp", {0xa941'0801U}, 16, 16},                // ldp x1,x2,[x0,#16]
      {"stp", {0xa901'0801U}, 16, 16},                // stp x1,x2,[x0,#16]
      {"ld1 16b", {0x4c40'7000U}, 0, 16},             // ld1 {v0.16b},[x0]
      {"st1 16b", {0x4c00'7000U}, 0, 16},             // st1 {v0.16b},[x0]
      {"ldr q", {0x3dc0'0401U}, 16, 16},              // ldr q1,[x0,#16]
      {"simd load run", {0x3dc0'0401U, 0x3dc0'0802U}, 16, 32},
      {"simd store run", {0x3d80'0401U, 0x3d80'0802U}, 16, 32},
      {"ldar", {0xc8df'fc01U}, 0, 8},                 // ldar x1,[x0]
      {"ldadd", {0xf821'0002U}, 0, 8, 1, 1, true},    // ldadd x1,x2,[x0]
      {"fused exclusive",
       {0xc85f'7c03U, 0x9100'0463U, 0xc804'7c03U, 0x35ff'ffa4U}, 0, 8, 1, 1,
       false, true},
              // ldxr x3,[x0]; add x3,x3,#1; stxr w4,x3,[x0]; cbnz w4,start
      {"dc zva", {0xd50b'7420U}, 0, 64, 1, 64},       // dc zva,x0
  };
  return result;
}

struct Outcome final {
  Event event{};
  std::size_t retired{};
  std::uint64_t fault_address{};
  arm64::CpuState cpu{};
  std::vector<std::byte> memory;
};

class Machine final {
public:
  Machine(unsigned long generation, const Family &family)
      : generation_{generation}, backing_{4},
        mapping_{generation, base, backing_, 4}, family_{family} {
    std::uint64_t offset = 0;
    for (const auto word : family.program) {
      backing_.put32(offset, word);
      offset += 4;
    }
    backing_.put32(offset, system_call);
    require(executor_.begin_execution(generation_, base));
  }

  ~Machine() { executor_.end_execution(); }

  [[nodiscard]] static std::vector<std::byte> pristine() {
    std::vector<std::byte> pattern(2U * page);
    for (std::size_t index = 0; index < pattern.size(); ++index)
      pattern[index] = static_cast<std::byte>(index * 37U + 11U);
    return pattern;
  }

  /* Runs the program once from fresh registers and data; `pointer` is x0 and
   * `sp` the stack pointer. */
  Outcome run(bool tbi, std::uint64_t pointer, std::uint64_t sp = 0,
              std::uint64_t pc = base) {
    backing_.put(page, pristine());
    arm64::CpuState cpu{};
    cpu.pc = pc;
    cpu.tbi_enabled = tbi;
    cpu.registers[0] = pointer;
    cpu.registers[1] = 0x1122'3344'5566'7788ULL;
    cpu.registers[2] = family_.index;
    cpu.registers[3] = 0x0102'0304'0506'0708ULL;
    cpu.registers[4] = 0x1112'1314'1516'1718ULL;
    cpu.registers[5] = 0x2122'2324'2526'2728ULL;
    cpu.sp = sp;
    for (std::size_t lane = 0; lane < 16; ++lane) {
      cpu.vectors[0].bytes[lane] = static_cast<std::byte>(0xa0U + lane);
      cpu.vectors[1].bytes[lane] = static_cast<std::byte>(0xb0U + lane);
      cpu.vectors[2].bytes[lane] = static_cast<std::byte>(0xc0U + lane);
    }
    Outcome outcome;
    Event event = Event::continued;
    for (unsigned blocks = 0; blocks < 8U && event == Event::continued;
         ++blocks) {
      std::size_t executed = 0;
      event = executor_.run_block(cpu, memory_, generation_, executed);
      outcome.retired += executed;
    }
    outcome.event = event;
    outcome.fault_address =
        event == Event::memory_fault ? executor_.fault_address() : 0;
    outcome.cpu = cpu;
    outcome.memory.resize(2U * page);
    backing_.get(page, outcome.memory);
    return outcome;
  }

private:
  unsigned long generation_;
  JitBacking backing_;
  JitMapping mapping_;
  JitExecutor executor_;
  StaticGuestMemory memory_;
  const Family &family_;
};

unsigned long next_generation = 14'000;

void fail(const Family &family, const char *what, std::uint64_t tag) {
  std::fprintf(stderr, "%s, tag %#llx: %s\n", family.name,
               static_cast<unsigned long long>(tag), what);
  require(false);
}

/* Whether `tagged` left the state `reference` did, and the base register its
 * tag. */
[[nodiscard]] bool same_state(const Family &family, const Outcome &reference,
                              const Outcome &tagged, std::uint64_t tag) {
  if (reference.event != tagged.event ||
      (!family.retries && reference.retired != tagged.retired) ||
      reference.memory != tagged.memory || reference.cpu.pc != tagged.cpu.pc) {
    std::fprintf(stderr, "event %d/%d retired %zu/%zu memory %s pc %#llx/%#llx\n",
                 static_cast<int>(reference.event),
                 static_cast<int>(tagged.event), reference.retired,
                 tagged.retired,
                 reference.memory == tagged.memory ? "same" : "differs",
                 static_cast<unsigned long long>(reference.cpu.pc),
                 static_cast<unsigned long long>(tagged.cpu.pc));
    for (std::size_t offset = 0; offset < reference.memory.size(); ++offset)
      if (reference.memory[offset] != tagged.memory[offset]) {
        std::fprintf(stderr, "first difference at %zu: %02x/%02x; x4 %#llx/%#llx\n",
                     offset, static_cast<unsigned>(reference.memory[offset]),
                     static_cast<unsigned>(tagged.memory[offset]),
                     static_cast<unsigned long long>(reference.cpu.registers[4]),
                     static_cast<unsigned long long>(tagged.cpu.registers[4]));
        break;
      }
    return false;
  }
  for (unsigned number = 1; number < 31; ++number)
    if (reference.cpu.registers[number] != tagged.cpu.registers[number]) {
      std::fprintf(stderr, "x%u %#llx/%#llx\n", number,
                   static_cast<unsigned long long>(
                       reference.cpu.registers[number]),
                   static_cast<unsigned long long>(
                       tagged.cpu.registers[number]));
      return false;
    }
  for (unsigned number = 0; number < 32; ++number)
    if (reference.cpu.vectors[number].bytes !=
        tagged.cpu.vectors[number].bytes)
      return false;
  return (reference.cpu.registers[0] & low_bits) ==
             (tagged.cpu.registers[0] & low_bits) &&
         (tagged.cpu.registers[0] & ~low_bits) == tag;
}

/* The address an access at `address` reports when it faults. */
[[nodiscard]] std::uint64_t reported(const Family &family,
                                     std::uint64_t address) {
  return (address + family.first_access) & ~(family.granule - 1U);
}

/* One family at one untagged pointer. */
void check_family(const Family &family, std::uint64_t pointer,
                  bool must_complete) {
  if (family.lse && !::uml::platform::apple::probed_host_capabilities().has(
                        ::uml::cpu::ArmFeature::lse))
    return;
  Machine machine{next_generation++, family};
  const auto reference = machine.run(false, pointer);
  if (must_complete && reference.event != Event::system_call)
    fail(family, "the untagged reference did not complete", 0);
  for (const auto tag : tags) {
    const auto tagged = pointer | tag;
    const auto with_tbi = machine.run(true, tagged);
    if (!same_state(family, reference, with_tbi, tag))
      fail(family, "a tagged pointer with TBI did not match the reference",
           tag);
    if (reference.event == Event::memory_fault &&
        with_tbi.fault_address != reference.fault_address)
      fail(family, "a fault reported another address under TBI", tag);

    /* The tag is part of the address without TBI: it faults before
     * anything moves. */
    const auto without = machine.run(false, tagged);
    if (without.event != Event::memory_fault ||
        without.fault_address != reported(family, tagged))
      fail(family, "a tagged pointer without TBI did not fault at itself",
           tag);
    if (without.memory != Machine::pristine())
      fail(family, "a tagged pointer without TBI moved memory", tag);

    /* The same executor, under TBI again, strips again. */
    if (!same_state(family, reference, machine.run(true, tagged), tag))
      fail(family, "TBI after a run without it did not match the reference",
           tag);

    /* An unmapped page faults at the untagged address under TBI. */
    const auto far_pointer = unmapped | tag;
    const auto fault = machine.run(true, far_pointer);
    if (fault.event != Event::memory_fault ||
        fault.fault_address != reported(family, unmapped))
      fail(family, "an unmapped tagged access reported a tagged address",
           tag);
  }
}

void every_family_ignores_the_tag() {
  for (const auto &family : families()) {
    /* In a page, aligned. */
    check_family(family, data + 0x100, true);
    /* Across the first data page's end into the second, when the access is
     * wide enough to straddle: its last byte is the third byte past. */
    if (family.span >= 8U)
      check_family(family,
                   data + page - family.first_access - family.span + 3U,
                   false);
  }
}

/* An SP base is no exception: under TBI a tagged SP reaches the byte its low
 * 56 bits name and keeps its tag, and a fault reports the untagged address.
 * Without TBI the tag is part of the address. */
void sp_is_untagged_like_any_base() {
  const std::array<Family, 7> sp_families{{
      {"str sp", {0xf900'07e1U}, 8, 8},               // str x1,[sp,#8]
      {"ldr sp", {0xf940'07e1U}, 8, 8},               // ldr x1,[sp,#8]
      {"stp sp", {0xa901'0be1U}, 16, 16},             // stp x1,x2,[sp,#16]
      {"ld1 sp", {0x4c40'73e0U}, 0, 16},              // ld1 {v0.16b},[sp]
      {"ldar sp", {0xc8df'ffe1U}, 0, 8},              // ldar x1,[sp]
      {"ldadd sp", {0xf821'03e2U}, 0, 8, 1, 1, true}, // ldadd x1,x2,[sp]
      {"fused exclusive sp",
       {0xc85f'7fe3U, 0x9100'0463U, 0xc804'7fe3U, 0x35ff'ffa4U}, 0, 8, 1, 1,
       false, true},
              // ldxr x3,[sp]; add x3,x3,#1; stxr w4,x3,[sp]; cbnz w4,start
  }};
  for (const auto &family : sp_families) {
    if (family.lse && !::uml::platform::apple::probed_host_capabilities().has(
                          ::uml::cpu::ArmFeature::lse))
      continue;
    Machine machine{next_generation++, family};
    const auto reference = machine.run(true, 0, data + 0x100);
    if (reference.event != Event::system_call)
      fail(family, "an untagged SP did not complete under TBI", 0);
    for (const auto tag : tags) {
      const auto tagged = (data + 0x100) | tag;
      const auto outcome = machine.run(true, 0, tagged);
      bool same = outcome.event == reference.event &&
                  (family.retries || outcome.retired == reference.retired) &&
                  outcome.memory == reference.memory &&
                  outcome.cpu.pc == reference.cpu.pc &&
                  outcome.cpu.sp == tagged;
      for (unsigned number = 0; number < 31; ++number)
        same = same &&
               outcome.cpu.registers[number] == reference.cpu.registers[number];
      for (unsigned number = 0; number < 32; ++number)
        same = same && outcome.cpu.vectors[number].bytes ==
                           reference.cpu.vectors[number].bytes;
      if (!same)
        fail(family, "a tagged SP with TBI did not match the reference", tag);

      /* Without TBI the tagged SP is an address nothing maps. */
      const auto without = machine.run(false, 0, tagged);
      if (without.event != Event::memory_fault ||
          without.fault_address != reported(family, tagged))
        fail(family, "a tagged SP without TBI did not fault at itself", tag);
      if (without.memory != Machine::pristine())
        fail(family, "a tagged SP without TBI moved memory", tag);

      /* An unmapped page faults at the untagged address. */
      const auto fault = machine.run(true, 0, unmapped | tag);
      if (fault.event != Event::memory_fault ||
          fault.fault_address != reported(family, unmapped))
        fail(family, "an unmapped tagged SP reported a tagged address", tag);
    }
  }
}

/* A PC with a bit the translation key keeps for its own state is no address:
 * it faults like any unmapped fetch and does not run the translation of the
 * PC without that bit. */
void keyed_pcs_are_not_addresses() {
  const Family family{"str unsigned", {0xf900'0401U}, 8, 8};
  for (const bool tbi : {false, true}) {
    Machine machine{next_generation++, family};
    /* Translate and cache the program under both modes. */
    require(machine.run(true, data + 0x100).event == Event::system_call);
    require(machine.run(false, data + 0x100).event == Event::system_call);
    for (const auto bit : {std::uint64_t{1} << 61, std::uint64_t{1} << 62,
                           std::uint64_t{1} << 63}) {
      const auto outcome =
          machine.run(tbi, data + 0x100, 0, base | bit);
      if (outcome.event != Event::memory_fault || outcome.retired != 0 ||
          outcome.fault_address != (base | bit))
        fail(family, "a PC with a key bit did not fault as a fetch", bit);
      if (outcome.memory != Machine::pristine())
        fail(family, "a PC with a key bit ran the translation of another", bit);
    }
  }
}

void key_carries_the_mode() {
  /* The key of a TBI translation differs from the key of the other, yields
   * the PC back, and is no AArch32 key. */
  const auto plain = arm64::translation_key(base, 0);
  const auto tagged = arm64::translation_key(base, arm64::translation_mode_tbi);
  require(plain == base && tagged != plain);
  require(arm64::translation_key_pc(tagged) == base);
  require(arm64::translation_key_mode(tagged) == arm64::translation_mode_tbi);
  require(arm64::translation_key_mode(plain) == 0);
  /* The SME mode is independent of it. */
  const auto both = arm64::translation_key(
      base, arm64::translation_mode_tbi | arm64::svcr_sm);
  require(arm64::translation_key_pc(both) == base);
  require(arm64::translation_key_mode(both) ==
          (arm64::translation_mode_tbi | arm64::svcr_sm));
}

}  // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  key_carries_the_mode();
  every_family_ignores_the_tag();
  sp_is_untagged_like_any_base();
  keyed_pcs_are_not_addresses();
  return 0;
}
