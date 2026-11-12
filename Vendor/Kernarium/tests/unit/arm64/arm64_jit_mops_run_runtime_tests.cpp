// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * FEAT_MOPS prologue, main and epilogue sequences, which the JIT translates
 * together: inline when the transfer is untagged and stays inside one
 * destination page and one source page, through the shared helper otherwise.
 * Both engines must produce the bytes of an independent model: memmove for
 * CPY in either direction, and for CPYF 64-byte blocks from the first byte,
 * each read completely before it is written. Each program first fills the
 * TLB for both pages with one-byte self-copies, so the JIT's inline arm runs.
 */
#include "arm64_engine_test_support.hpp"
#include "jit_test_support.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;

namespace {

constexpr auto page_size = std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE};
constexpr auto code_base = engine::engine_test_code_base;
constexpr auto data_base = engine::engine_test_data_base;
constexpr std::uint32_t system_call = 0xd400'0001U;

enum class Family {
  copy_forward,
  copy,
  set,
};

[[nodiscard]] constexpr std::uint32_t
mops_word(Family family, unsigned stage, unsigned destination,
          unsigned source, unsigned count, unsigned options) noexcept {
  if (family == Family::set)
    return 0x19c0'0400U | (stage << 14) | (source << 16) |
           ((options & 3U) << 12) | (count << 5) | destination;
  return 0x1900'0400U | ((family == Family::copy ? 1U : 0U) << 26) |
         (stage << 22) | (source << 16) | ((options & 15U) << 12) |
         (count << 5) | destination;
}

void append_sequence(std::vector<std::uint32_t> &words, Family family,
                     unsigned destination, unsigned source, unsigned count,
                     unsigned options = 0) {
  for (unsigned stage = 0; stage < 3; ++stage)
    words.push_back(
        mops_word(family, stage, destination, source, count, options));
}

[[nodiscard]] std::vector<std::byte> initial_bytes(std::size_t size) {
  std::vector<std::byte> bytes(size);
  std::uint32_t state = 0x4d4f'5053U;
  for (auto &byte : bytes) {
    state = state * 1664525U + 1013904223U;
    byte = static_cast<std::byte>(state >> 24);
  }
  return bytes;
}

/* Applies one operation to bytes indexed from data_base and returns whether
 * the copy runs backward. */
bool apply_model(std::vector<std::byte> &bytes, Family family,
                 std::uint64_t destination, std::uint64_t source,
                 std::uint64_t count, std::byte fill) {
  const auto to = destination - data_base;
  if (family == Family::set) {
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(to), count, fill);
    return false;
  }
  const auto from = source - data_base;
  const bool overlaps_above = from < to && to < from + count;
  if (family == Family::copy_forward && overlaps_above) {
    for (std::uint64_t done = 0; done < count;) {
      const auto size = std::min<std::uint64_t>(64, count - done);
      std::vector<std::byte> block(
          bytes.begin() + static_cast<std::ptrdiff_t>(from + done),
          bytes.begin() + static_cast<std::ptrdiff_t>(from + done + size));
      std::copy(block.begin(), block.end(),
                bytes.begin() + static_cast<std::ptrdiff_t>(to + done));
      done += size;
    }
    return false;
  }
  std::vector<std::byte> copied(
      bytes.begin() + static_cast<std::ptrdiff_t>(from),
      bytes.begin() + static_cast<std::ptrdiff_t>(from + count));
  std::copy(copied.begin(), copied.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(to));
  return family == Family::copy && overlaps_above;
}

struct Operation final {
  Family family;
  unsigned destination;
  unsigned source;
  unsigned count;
  std::uint64_t destination_address;
  std::uint64_t source_address;
  std::uint64_t bytes;
  std::uint64_t fill{0xa5};
  unsigned options{};
  /* Live flags and registers around the sequence. */
  bool context{};
};

[[nodiscard]] const char *family_name(Family family) noexcept {
  return family == Family::copy_forward ? "CPYF"
         : family == Family::copy       ? "CPY"
                                        : "SET";
}

void check(engine::EngineHarness &harness, const Operation &operation) {
  const auto destination_page =
      operation.destination_address & ~(page_size - 1U);
  const auto source_page =
      (operation.family == Family::set ? operation.destination_address
                                       : operation.source_address) &
      ~(page_size - 1U);
  std::vector<std::uint32_t> words;
  append_sequence(words, Family::copy_forward, 20, 21, 22);
  append_sequence(words, Family::copy_forward, 23, 24, 25);
  if (operation.context)
    words.push_back(0xb100'04e7U); // adds x7, x7, #1
  append_sequence(words, operation.family, operation.destination,
                  operation.source, operation.count, operation.options);
  if (operation.context) {
    words.push_back(0x9a9f'37e6U); // cset x6, hs
    words.push_back(0x8b00'0007U | (operation.count << 16) |
                    (operation.destination << 5)); // add x7, xd, xn
  }
  words.push_back(system_call);
  std::vector<engine::ProgramWord> code;
  for (std::size_t index = 0; index < words.size(); ++index)
    code.push_back({index * sizeof(std::uint32_t), words[index]});

  arm64::CpuState initial{};
  initial.nzcv = 0xd000'0000U;
  initial.registers[7] = 5;
  initial.registers[20] = initial.registers[21] = destination_page + 0x40U;
  initial.registers[22] = 1;
  initial.registers[23] = initial.registers[24] = source_page + 0x60U;
  initial.registers[25] = 1;
  initial.registers[operation.destination] = operation.destination_address;
  initial.registers[operation.count] = operation.bytes;
  if (operation.source != 31U)
    initial.registers[operation.source] =
        operation.family == Family::set ? operation.fill
                                        : operation.source_address;

  const auto before = initial_bytes(2U * page_size);
  auto after = before;
  const auto fill = static_cast<std::byte>(
      operation.source == 31U ? 0U : operation.fill & 0xffU);
  const bool backward =
      apply_model(after, operation.family, operation.destination_address,
                  operation.source_address, operation.bytes, fill);

  auto expected = initial;
  expected.pc = code_base + words.size() * sizeof(std::uint32_t);
  expected.registers[20] = expected.registers[21] = destination_page + 0x41U;
  expected.registers[22] = 0;
  expected.registers[23] = expected.registers[24] = source_page + 0x61U;
  expected.registers[25] = 0;
  expected.registers[operation.count] = 0;
  if (!backward) {
    expected.registers[operation.destination] += operation.bytes;
    if (operation.family != Family::set)
      expected.registers[operation.source] += operation.bytes;
  }
  expected.nzcv = backward ? 0xa000'0000U : 0x2000'0000U;
  if (operation.context) {
    expected.registers[6] = 1;
    expected.registers[7] = expected.registers[operation.destination];
  }

  const std::array regions{engine::MemoryRegion{
      .address = data_base,
      .initial = std::span<const std::byte>{before},
      .expected = std::span<const std::byte>{after},
  }};
  const auto name = std::string{"fused "} + family_name(operation.family) +
                    " n=" + std::to_string(operation.bytes) + " dst+" +
                    std::to_string(operation.destination_address - data_base) +
                    (operation.family == Family::set
                         ? std::string{}
                         : " src+" + std::to_string(operation.source_address -
                                                    data_base)) +
                    (operation.context ? " in context" : "") +
                    (operation.options != 0 ? " with hints" : "");
  harness.check_program({
      .name = name,
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = engine::NativeRequirements::baseline(),
      .memory = regions,
  });
}

void sizes_and_registers(engine::EngineHarness &harness) {
  constexpr std::array<std::array<unsigned, 3>, 5> registers{{
      {3, 4, 5}, {0, 1, 2}, {28, 29, 30}, {10, 8, 11}, {17, 12, 13}}};
  constexpr std::array<std::uint64_t, 19> sizes{
      0, 1, 2, 3, 5, 8, 15, 16, 31, 32, 33, 63, 64, 65, 127, 128, 300,
      1000, page_size};
  for (const auto family : {Family::copy_forward, Family::copy, Family::set})
    for (std::size_t index = 0; index < sizes.size(); ++index) {
      const auto &set = registers[index % registers.size()];
      const auto bytes = sizes[index];
      const auto slack = std::min<std::uint64_t>(32, page_size - bytes);
      check(harness, {
                         .family = family,
                         .destination = set[0],
                         .source = set[1],
                         .count = set[2],
                         .destination_address =
                             data_base + page_size + (index * 7U) % (slack + 1U),
                         .source_address = data_base + (index * 5U) % (slack + 1U),
                         .bytes = bytes,
                         .fill = 0x1234'5678'9abc'de00ULL | index,
                     });
    }
}

void live_state_and_hints(engine::EngineHarness &harness) {
  for (const auto family : {Family::copy_forward, Family::copy, Family::set}) {
    for (const std::uint64_t bytes : {0U, 13U, 64U, 300U})
      check(harness, {.family = family, .destination = 3, .source = 4,
                      .count = 5,
                      .destination_address = data_base + page_size + 0x200U,
                      .source_address = data_base + 0x200U, .bytes = bytes,
                      .context = true});
    check(harness, {.family = family, .destination = 3, .source = 4,
                    .count = 5,
                    .destination_address = data_base + page_size + 0x40U,
                    .source_address = data_base + 0x80U, .bytes = 300,
                    .options = family == Family::set ? 3U : 15U});
  }
  check(harness, {.family = Family::set, .destination = 3, .source = 31,
                  .count = 5, .destination_address = data_base + 0x100U,
                  .source_address = 0, .bytes = 65});
  check(harness, {.family = Family::set, .destination = 26, .source = 31,
                  .count = 27, .destination_address = data_base + 0x140U,
                  .source_address = 0, .bytes = 1000});
}

/* The helper arm: a page boundary inside either range, or more than a page. */
void page_boundaries(engine::EngineHarness &harness) {
  for (const auto family : {Family::copy_forward, Family::copy, Family::set}) {
    check(harness, {.family = family, .destination = 3, .source = 4,
                    .count = 5,
                    .destination_address = data_base + page_size - 100U,
                    .source_address = data_base + page_size + 0x100U,
                    .bytes = 300});
    check(harness, {.family = family, .destination = 3, .source = 4,
                    .count = 5,
                    .destination_address = data_base + page_size - 64U,
                    .source_address = data_base + 0x300U, .bytes = 64});
  }
  for (const auto family : {Family::copy_forward, Family::copy})
    check(harness, {.family = family, .destination = 3, .source = 4,
                    .count = 5,
                    .destination_address = data_base + page_size + 0x100U,
                    .source_address = data_base + page_size - 50U,
                    .bytes = 300});
  check(harness, {.family = Family::set, .destination = 3, .source = 4,
                  .count = 5, .destination_address = data_base,
                  .source_address = 0, .bytes = page_size + 1U});
}

/* Overlap inside one page: CPYF blocks read earlier blocks' results, a CPY
 * whose destination is above its source runs backward. */
void overlaps(engine::EngineHarness &harness) {
  constexpr std::array<std::pair<int, std::uint64_t>, 16> cases{{
      {1, 20}, {-1, 20}, {1, 64}, {63, 64}, {-63, 64}, {1, 65}, {-1, 65},
      {17, 300}, {-17, 300}, {64, 300}, {-64, 300}, {65, 1000},
      {-65, 1000}, {100, 1000}, {-100, 1000}, {0, 1000}}};
  for (const auto family : {Family::copy_forward, Family::copy})
    for (const auto &[delta, bytes] : cases)
      check(harness, {.family = family, .destination = 3, .source = 4,
                      .count = 5,
                      .destination_address = static_cast<std::uint64_t>(
                          static_cast<std::int64_t>(data_base + 0x600U) +
                          delta),
                      .source_address = data_base + 0x600U, .bytes = bytes});
}

[[nodiscard]] constexpr std::uint32_t
load_exclusive_byte(unsigned base, unsigned transfer) noexcept {
  return 0x085f'7c00U | (base << 5U) | transfer;
}

[[nodiscard]] constexpr std::uint32_t
store_exclusive_byte(unsigned status, unsigned base,
                     unsigned transfer) noexcept {
  return 0x0800'7c00U | (status << 16U) | (base << 5U) | transfer;
}

/*
 * While another vCPU holds a software reservation, a JIT block that stores
 * runs its tracked translation, which records each inline transfer's range.
 * A reservation inside the transferred range is lost; one outside survives.
 * The memory is zero, so every store rewrites the value the reservation
 * holds and only the recorded range can fail the store-exclusive.
 */
void tracked_reservation_case(Family family, bool inside,
                              unsigned case_index) {
  const auto generation = static_cast<unsigned long>(9800U + case_index);
  const auto base = std::uint64_t{0xf0'0000} + case_index * 4U * page_size;
  const auto destination_page = base + page_size;
  const auto source_page = base + 2U * page_size;
  /* Backward CPY overlaps inside the destination page. */
  const bool backward = family == Family::copy;
  const auto source = backward ? destination_page + 0x100U
                               : source_page + 0x100U;
  const auto destination = backward ? destination_page + 0x111U
                                    : destination_page + 0x100U;
  constexpr std::uint64_t bytes = 300;
  const auto reservation =
      inside ? destination + bytes - 7U : destination_page + 0x400U;

  engine::JitBacking backing{3};
  std::vector<std::uint32_t> second;
  append_sequence(second, Family::copy_forward, 20, 21, 22);
  append_sequence(second, Family::copy_forward, 23, 24, 25);
  append_sequence(second, family, 3, family == Family::set ? 31U : 4U, 5);
  second.push_back(system_call);
  backing.put32(0, load_exclusive_byte(1, 5));
  backing.put32(4, system_call);
  backing.put32(8, store_exclusive_byte(6, 1, 4));
  backing.put32(12, system_call);
  for (std::size_t index = 0; index < second.size(); ++index)
    backing.put32(16U + index * sizeof(std::uint32_t), second[index]);
  const std::vector<std::byte> data(2U * page_size);
  backing.put(page_size, data);
  engine::JitMapping mapping{generation, base, backing, 3};

  uml::cpu::arm64::JitExecutor first;
  uml::cpu::arm64::JitExecutor other;
  uml::memory::StaticGuestMemory shared_memory;
  arm64::CpuState first_cpu{};
  arm64::CpuState other_cpu{};
  first_cpu.pc = base;
  first_cpu.registers[1] = reservation;
  first_cpu.registers[4] = 0xc7;
  other_cpu.pc = base + 16U;
  other_cpu.registers[20] = other_cpu.registers[21] = destination_page + 0x40U;
  other_cpu.registers[22] = 1;
  other_cpu.registers[23] = other_cpu.registers[24] = source_page + 0x60U;
  other_cpu.registers[25] = 1;
  other_cpu.registers[3] = destination;
  other_cpu.registers[4] = source;
  other_cpu.registers[5] = bytes;

  engine::require(first.begin_execution(generation, first_cpu.pc));
  std::size_t executed = 0;
  engine::require(first.run_block(first_cpu, shared_memory, generation,
                                  executed) ==
                  ::uml::cpu::EngineEvent::continued);
  engine::require(first_cpu.exclusive_size == 1);

  engine::require(other.begin_execution(generation, other_cpu.pc));
  for (unsigned step = 0; step < 8; ++step) {
    const auto event =
        other.run_block(other_cpu, shared_memory, generation, executed);
    if (event == ::uml::cpu::EngineEvent::system_call)
      break;
    engine::require(event == ::uml::cpu::EngineEvent::continued);
  }
  engine::require(other_cpu.pc ==
                  base + 16U + second.size() * sizeof(std::uint32_t));
  engine::require(other_cpu.registers[5] == 0);
  other.end_execution();

  std::vector<std::byte> actual(data.size());
  backing.get(page_size, actual);
  engine::require(actual == data);
  engine::require(other_cpu.registers[3] ==
                  (backward ? destination : destination + bytes));

  first_cpu.pc = base + 8U;
  engine::require(first.run_block(first_cpu, shared_memory, generation,
                                  executed) ==
                  ::uml::cpu::EngineEvent::continued);
  engine::require(first_cpu.registers[6] == (inside ? 1U : 0U));
  first.end_execution();
}

void tracked_transfers_invalidate_reservations() {
  unsigned index = 0;
  for (const auto family : {Family::copy_forward, Family::copy, Family::set})
    for (const bool inside : {true, false})
      tracked_reservation_case(family, inside, index++);
}

} // namespace

int main() {
  engine::require(arm64::set_jit_cache_budget(256U * 1024U * 1024U, 3));
  engine::EngineHarness harness;
  sizes_and_registers(harness);
  live_state_and_hints(harness);
  page_boundaries(harness);
  overlaps(harness);
  const auto &counts = harness.counts();
  engine::require(counts.gadgets != 0);
  engine::require(counts.jit == counts.gadgets);
  engine::require(counts.jit_skipped == 0);
  engine::require(counts.jit_unavailable == 0);
  harness.print_summary("arm64-jit-mops-run-runtime");
  tracked_transfers_invalidate_reservations();
  return 0;
}
