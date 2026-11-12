// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/jit/jit_semantic_instructions.hpp"
#include "require.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>

/*
 * The semantic store-exclusive completes with a compare-and-swap from the
 * value its load-exclusive read. A write that does not take the exclusive
 * lock (the guest kernel's futex and SWP emulation, DMA) and lands after the
 * store-exclusive's check must fail the store-exclusive and survive, not be
 * overwritten. The memory below makes that write land at the one point where
 * the race is otherwise too narrow to pin: when the store-exclusive asks for
 * the location's write pointer, which is after its check of the monitor and
 * of the value and before its store.
 */

using uml::testing::require;
using uml::cpu::arm64::CpuState;
using uml::memory::MemoryAccess;
namespace sem = uml::cpu::arm64::jit::semantic;

namespace {

class InterferingMemory final : public uml::cpu::arm64::GuestMemory {
public:
  static constexpr std::uint64_t base = 0x40'0000U;

  [[nodiscard]] bool read(std::uint64_t address,
                          std::span<std::byte> destination) override {
    if (!contains(address, destination.size()))
      return false;
    std::memcpy(destination.data(), bytes_.data() + (address - base),
                destination.size());
    return true;
  }

  [[nodiscard]] bool write(std::uint64_t address,
                           std::span<const std::byte> source) override {
    if (!contains(address, source.size()))
      return false;
    std::memcpy(bytes_.data() + (address - base), source.data(),
                source.size());
    return true;
  }

  [[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
                                          std::size_t size,
                                          MemoryAccess access) override {
    if (!contains(address, size))
      return nullptr;
    if (access == MemoryAccess::write && interference_) {
      const auto interfere = std::move(interference_);
      interference_ = nullptr;
      interfere();
    }
    return bytes_.data() + (address - base);
  }

  /* The next request for a write pointer first runs `write`, as another
   * agent's store. */
  void interfere_once(std::function<void()> write) {
    interference_ = std::move(write);
  }

  [[nodiscard]] std::uint64_t get64(std::size_t offset) const {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes_.data() + offset, sizeof value);
    return value;
  }

  void put64(std::size_t offset, std::uint64_t value) {
    std::memcpy(bytes_.data() + offset, &value, sizeof value);
  }

  /* The write of an agent outside the guest's exclusive lock. */
  void outside_write64(std::size_t offset, std::uint64_t value) {
    std::atomic_ref<std::uint64_t>{*reinterpret_cast<std::uint64_t *>(
        bytes_.data() + offset)}.store(value, std::memory_order_seq_cst);
  }

private:
  [[nodiscard]] static bool contains(std::uint64_t address,
                                     std::size_t size) {
    return address >= base && address - base + size <= 64U;
  }

  alignas(16) std::array<std::byte, 64> bytes_{};
  std::function<void()> interference_;
};

struct Form final {
  const char *name;
  std::uint32_t load;
  std::uint32_t store;
  unsigned bytes;      // monitored bytes
  bool pair;
};

/* Each loads x3/w3 (or the pair x2, x3) from [x1] and stores x4/w4 (or the
 * pair x6, x7) through [x1] with status w5. */
constexpr Form forms[] = {
    {"ldxrb/stxrb", 0x085f'7c23U, 0x0805'7c24U, 1, false},
    {"ldxrh/stxrh", 0x485f'7c23U, 0x4805'7c24U, 2, false},
    {"ldxr w/stxr w", 0x885f'7c23U, 0x8805'7c24U, 4, false},
    {"ldxr x/stxr x", 0xc85f'7c23U, 0xc805'7c24U, 8, false},
    {"ldxp w/stxp w", 0x887f'0c22U, 0x8825'1c26U, 8, true},
    {"ldxp x/stxp x", 0xc87f'0c22U, 0xc825'1c26U, 16, true},
};

constexpr std::uint64_t old_low = 0x1122'3344'5566'7788U;
constexpr std::uint64_t old_high = 0x99aa'bbcc'ddee'ff00U;
constexpr std::uint64_t outside_low = 0x0123'4567'89ab'cdefU;
constexpr std::uint64_t outside_high = 0xfedc'ba98'7654'3210U;

[[nodiscard]] std::uint64_t mask(unsigned bytes) {
  return bytes >= 8 ? ~std::uint64_t{0}
                    : (std::uint64_t{1} << (bytes * 8)) - 1;
}

void prepare(CpuState &cpu, InterferingMemory &memory, const Form &form) {
  memory.put64(0, old_low);
  memory.put64(8, old_high);
  cpu.registers[1] = InterferingMemory::base;
  cpu.registers[4] = 0xa5a5'a5a5'a5a5'a5a5U;
  cpu.registers[6] = 0x6666'6666'6666'6666U;
  cpu.registers[7] = 0x7777'7777'7777'7777U;
  cpu.registers[5] = 0xdeadU;
  const auto loaded = sem::load_store_exclusive(cpu, memory, form.load);
  require(loaded.reason == sem::StopReason::continued, form.name);
  require(cpu.exclusive_size == form.bytes, form.name);
}

void store_sticks_without_interference(const Form &form) {
  InterferingMemory memory;
  CpuState cpu{};
  prepare(cpu, memory, form);
  const auto stored = sem::load_store_exclusive(cpu, memory, form.store);
  require(stored.reason == sem::StopReason::continued, form.name);
  require(cpu.registers[5] == 0, form.name);
  require(cpu.exclusive_size == 0, form.name);
  if (form.pair && form.bytes == 16) {
    require(memory.get64(0) == 0x6666'6666'6666'6666U, form.name);
    require(memory.get64(8) == 0x7777'7777'7777'7777U, form.name);
  } else if (form.pair) {
    require(memory.get64(0) == 0x7777'7777'6666'6666U &&
                memory.get64(8) == old_high, form.name);
  } else {
    const auto kept = old_low & ~mask(form.bytes);
    require(memory.get64(0) == (kept | (cpu.registers[4] & mask(form.bytes))),
            form.name);
    require(memory.get64(8) == old_high, form.name);
  }
}

/* The outside write changes the first monitored element (or, for the
 * 16-byte pair, the second): the store-exclusive fails, the write stays. */
void outside_write_fails_the_store(const Form &form, bool second_element) {
  InterferingMemory memory;
  CpuState cpu{};
  prepare(cpu, memory, form);
  std::uint64_t expected_low = old_low;
  std::uint64_t expected_high = old_high;
  if (second_element && form.bytes == 16) {
    expected_high = outside_high;
    memory.interfere_once([&] { memory.outside_write64(8, outside_high); });
  } else {
    /* Change a different value than the one read in the low bytes the form
     * monitors: the whole word for a single, the high half for the 8-byte
     * pair. */
    expected_low = form.pair && form.bytes == 8
                       ? (old_low & 0xffff'ffffU) | (outside_low & ~mask(4))
                       : (old_low & ~mask(form.bytes)) |
                             (outside_low & mask(form.bytes));
    require(expected_low != old_low, form.name);
    memory.interfere_once(
        [&] { memory.outside_write64(0, expected_low); });
  }
  const auto stored = sem::load_store_exclusive(cpu, memory, form.store);
  require(stored.reason == sem::StopReason::continued, form.name);
  require(cpu.registers[5] == 1, form.name);
  require(cpu.exclusive_size == 0, form.name);
  require(memory.get64(0) == expected_low, form.name);
  require(memory.get64(8) == expected_high, form.name);
}

} // namespace

int main() {
  for (const auto &form : forms) {
    store_sticks_without_interference(form);
    outside_write_fails_the_store(form, false);
    if (form.bytes == 16)
      outside_write_fails_the_store(form, true);
  }
  return 0;
}
