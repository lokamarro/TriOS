// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_compiler_families.hpp"
#include "jit_emitter.hpp"
#include "jit_linking.hpp"
#include "jit_runtime_contract.hpp"

#include "cpu/arm64/detail/bfloat_multiply.hpp"
#include "cpu/arm64/detail/half_fused_long.hpp"

#include <algorithm>

namespace uml::cpu::arm64::jit {

void JitCompilerFamilies::attach_family_emitter(void *emitter,
                                                bool *native_fp,
                                                bool enable_gpr_cache) noexcept {
  family_emitter_ = emitter;
  family_native_fp_ = native_fp;
  gpr_cache_enabled_ = enable_gpr_cache;
}

void JitCompilerFamilies::attach_link_resolver(
    void *context, LinkResolver resolver) noexcept {
  link_context_ = context;
  link_resolver_ = resolver;
}

void JitCompilerFamilies::begin_link_observation() noexcept {
  direct_link_refusals_ = 0;
  indirect_link_refusals_ = 0;
}

std::size_t JitCompilerFamilies::direct_link_refusals() const noexcept {
  return direct_link_refusals_;
}

std::size_t JitCompilerFamilies::indirect_link_refusals() const noexcept {
  return indirect_link_refusals_;
}

void JitCompilerFamilies::attach_indirect_link_resolver(
    void *context, IndirectLinkResolver resolver) noexcept {
  indirect_link_context_ = context;
  indirect_link_resolver_ = resolver;
}

bool JitCompilerFamilies::gpr_slots_unbound() const noexcept {
  return gpr_cache_enabled_ &&
         std::ranges::all_of(gpr_slots_, [](const auto &slot) {
           return slot.guest == unbound_guest;
         });
}

void JitCompilerFamilies::emit(std::uint32_t instruction) {
  static_cast<Arm64Emitter *>(family_emitter_)->emit(instruction);
}

JitCompilerFamilies::Label JitCompilerFamilies::make_label() {
  return static_cast<Arm64Emitter *>(family_emitter_)->make_label();
}

void JitCompilerFamilies::bind(Label label) {
  static_cast<Arm64Emitter *>(family_emitter_)->bind(label);
}

void JitCompilerFamilies::branch_condition(Label label, unsigned condition) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->branch_condition(label, condition);
}

void JitCompilerFamilies::compare_zero(Label label, unsigned reg,
                                       bool is_64_bit, bool nonzero) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->compare_zero(label, reg, is_64_bit, nonzero);
}

void JitCompilerFamilies::test_zero(Label label, unsigned reg, unsigned bit,
                                    bool nonzero) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->test_zero(label, reg, bit, nonzero);
}

void JitCompilerFamilies::load_constant(unsigned destination,
                                        std::uint64_t value) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->load_constant(destination, value);
}

void JitCompilerFamilies::load_literal_constant(unsigned destination,
                                                std::uint64_t value) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->load_literal_constant(destination, value);
}

void JitCompilerFamilies::load_x(unsigned destination, unsigned base,
                                 std::size_t offset) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->load_x(destination, base, offset);
}

void JitCompilerFamilies::store_x(unsigned source, unsigned base,
                                  std::size_t offset) {
  static_cast<Arm64Emitter *>(family_emitter_)->store_x(source, base, offset);
}

void JitCompilerFamilies::load_q(unsigned destination, unsigned base,
                                 std::size_t offset) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  constexpr auto vectors = offsetof(arm64::CpuState, vectors);
  constexpr auto vector_bytes =
      sizeof(arm64::CpuState::vectors);
  if (gpr_cache_enabled_ && base == 15U && offset >= vectors &&
      offset < vectors + vector_bytes &&
      ((offset - vectors) % sizeof(arm64::VectorRegister)) == 0U) {
    const auto guest = static_cast<unsigned>(
        (offset - vectors) / sizeof(arm64::VectorRegister));
    const auto slot = find_vector_slot(guest);
    if (slot) {
      auto &binding = vector_slots_[*slot];
      if (!binding.loaded) {
        emitter.load_q(resident_vector_hosts[*slot], 15, offset);
        binding.loaded = true;
      }
      if (resident_vector_hosts[*slot] != destination)
        emitter.move_q(destination, resident_vector_hosts[*slot]);
      return;
    }
  }
  emitter.load_q(destination, base, offset);
}

void JitCompilerFamilies::store_q(unsigned source, unsigned base,
                                  std::size_t offset) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  constexpr auto vectors = offsetof(arm64::CpuState, vectors);
  constexpr auto vector_bytes =
      sizeof(arm64::CpuState::vectors);
  if (gpr_cache_enabled_ && base == 15U && offset >= vectors &&
      offset < vectors + vector_bytes &&
      ((offset - vectors) % sizeof(arm64::VectorRegister)) == 0U) {
    const auto guest = static_cast<unsigned>(
        (offset - vectors) / sizeof(arm64::VectorRegister));
    const auto slot = find_vector_slot(guest);
    if (slot) {
      if (resident_vector_hosts[*slot] != source)
        emitter.move_q(resident_vector_hosts[*slot], source);
      vector_slots_[*slot].loaded = true;
      vector_slots_[*slot].dirty = true;
      return;
    }
  }
  emitter.store_q(source, base, offset);
}

void JitCompilerFamilies::return_constant(Event event,
                                          std::size_t instructions) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->return_constant(event, instructions);
}

void JitCompilerFamilies::return_dynamic(std::size_t instructions) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->return_dynamic(instructions);
}

std::optional<std::size_t>
JitCompilerFamilies::find_gpr_slot(unsigned guest,
                                   bool stack_pointer) const noexcept {
  if (!gpr_cache_enabled_ || (guest == 31U && !stack_pointer))
    return std::nullopt;
  stack_pointer = guest == 31U && stack_pointer;
  for (std::size_t index = 0; index < gpr_slots_.size(); ++index) {
    const auto &slot = gpr_slots_[index];
    if (slot.guest == guest && slot.stack_pointer == stack_pointer)
      return index;
  }
  return std::nullopt;
}

std::optional<std::size_t>
JitCompilerFamilies::allocate_gpr_slot(unsigned guest,
                                       bool stack_pointer) {
  if (!gpr_cache_enabled_ || (guest == 31U && !stack_pointer))
    return std::nullopt;
  stack_pointer = guest == 31U && stack_pointer;
  for (std::size_t index = 0; index < gpr_host_limit_; ++index) {
    auto &slot = gpr_slots_[index];
    /* A ZA slice index names W12-W15, so a streaming translation keeps
     * x12 free for the guest index. */
    if (slot.guest != unbound_guest ||
        (streaming_translation() && resident_gpr_hosts[index] == 12U))
      continue;
    slot.guest = guest;
    slot.stack_pointer = stack_pointer;
    return index;
  }
  return std::nullopt;
}

JitCompilerFamilies::NativeGpr
JitCompilerFamilies::read_native_gpr(unsigned guest, bool stack_pointer,
                                     unsigned fallback) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  if (guest == 31U && !stack_pointer)
    return {31U, true};
  auto slot = find_gpr_slot(guest, stack_pointer);
  if (!slot)
    slot = allocate_gpr_slot(guest, stack_pointer);
  if (slot) {
    auto &binding = gpr_slots_[*slot];
    if (!binding.loaded) {
      emitter.load_x(resident_gpr_hosts[*slot], 15,
                     register_offset(guest, stack_pointer));
      binding.loaded = true;
    }
    return {resident_gpr_hosts[*slot], true};
  }
  emitter.load_x(fallback, 15, register_offset(guest, stack_pointer));
  return {fallback, false};
}

JitCompilerFamilies::NativeGpr
JitCompilerFamilies::write_native_gpr(unsigned guest, bool stack_pointer,
                                      bool reads_old, unsigned fallback) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  if (guest == 31U && !stack_pointer)
    return {31U, true};
  auto slot = find_gpr_slot(guest, stack_pointer);
  if (!slot)
    slot = allocate_gpr_slot(guest, stack_pointer);
  if (slot) {
    auto &binding = gpr_slots_[*slot];
    if (reads_old && !binding.loaded)
      emitter.load_x(resident_gpr_hosts[*slot], 15,
                     register_offset(guest, stack_pointer));
    binding.loaded = true;
    binding.dirty = true;
    return {resident_gpr_hosts[*slot], true};
  }
  if (reads_old)
    emitter.load_x(fallback, 15, register_offset(guest, stack_pointer));
  return {fallback, false};
}

void JitCompilerFamilies::commit_native_gpr(NativeGpr value, unsigned guest,
                                            bool stack_pointer) {
  if (guest == 31U && !stack_pointer)
    return;
  if (!value.resident)
    static_cast<Arm64Emitter *>(family_emitter_)
        ->store_x(value.host, 15, register_offset(guest, stack_pointer));
}

std::optional<std::size_t>
JitCompilerFamilies::find_vector_slot(unsigned guest) const noexcept {
  if (!gpr_cache_enabled_)
    return std::nullopt;
  for (std::size_t index = 0; index < vector_slots_.size(); ++index)
    if (vector_slots_[index].guest == guest)
      return index;
  return std::nullopt;
}

std::optional<unsigned> JitCompilerFamilies::constrained_vector_source(
    std::uint32_t instruction) const noexcept {
  const auto &host = host_capabilities();

  if (!floating_or_vector_primary_class(instruction))
    return std::nullopt;

  const auto half_long =
      arm64::detail::decode_half_fused_long(instruction);
  /* Must agree exactly with emit_half_fused_long's predicate, or the register
   * constraint and the emitter disagree about which instructions are native. */
  if (half_long.recognized && half_long.by_element &&
      host.has(ArmFeature::fhm))
    return (instruction >> 16U) & 15U;

  const auto bfloat_long =
      arm64::detail::decode_bfloat_multiply_long(instruction);
  if (bfloat_long.recognized && bfloat_long.by_element &&
      host.has(ArmFeature::bf16))
    return (instruction >> 16U) & 15U;

  const unsigned size = (instruction >> 22U) & 3U;
  if (size == 1U && valid_simd_integer_indexed(instruction) &&
      host.advanced_simd) {
    const bool unsigned_group =
        (instruction & 0x2000'0000U) != 0U;
    const unsigned operation = (instruction >> 12U) & 15U;
    const bool requires_rdm =
        unsigned_group && (operation == 13U || operation == 15U);
    if (!requires_rdm || host.has(ArmFeature::rdm))
      return (instruction >> 16U) & 15U;
  }

  if (size == 0U && valid_simd_floating_indexed(instruction) &&
      host.floating_point && host.advanced_simd && host.fp16)
    return (instruction >> 16U) & 15U;

  return std::nullopt;
}

void JitCompilerFamilies::pin_constrained_vector(unsigned guest) noexcept {
  if (!gpr_cache_enabled_ || guest >= 32U)
    return;
  if (find_vector_slot(guest))
    return;
  for (std::size_t index = 0; index < pinned_vector_slot_count; ++index) {
    auto &slot = vector_slots_[index];
    if (slot.guest != unbound_guest)
      continue;
    slot.guest = guest;
    return;
  }
}

std::optional<JitCompilerFamilies::ConsecutiveVectorList>
JitCompilerFamilies::consecutive_vector_list(
    std::uint32_t instruction) const noexcept {
  const auto &host = host_capabilities();
  if (!host.advanced_simd)
    return std::nullopt;

  if ((instruction & 0xbfe0'8c00U) != 0x0e00'0000U)
    return std::nullopt;
  const unsigned count = ((instruction >> 13U) & 3U) + 1U;
  if (count == 1U)
    return std::nullopt;
  return ConsecutiveVectorList{
      .first = (instruction >> 5U) & 31U,
      .count = count,
  };
}

bool JitCompilerFamilies::pin_consecutive_vector_list(
    unsigned first, unsigned count) noexcept {
  if (!gpr_cache_enabled_ || first >= 32U || count < 2U ||
      count > 4U || count > vector_slots_.size() - pinned_vector_slot_count)
    return false;

  /*
   * q6/q7 remain dedicated to the more restrictive indexed encodings.
   * Search q16-q31 from the high end so ordinary first-use allocation keeps
   * its existing q16-up order. Every compatible binding is resolved here;
   * the generated table operation contains no staging decision or branch.
   */
  const auto first_start = vector_slots_.size() - count;
  for (std::size_t start = first_start + 1U;
       start-- > pinned_vector_slot_count;) {
    bool compatible = true;
    for (unsigned member = 0; member < count; ++member) {
      const unsigned guest = (first + member) & 31U;
      const auto existing = find_vector_slot(guest);
      if ((existing && *existing != start + member) ||
          (vector_slots_[start + member].guest != unbound_guest &&
           vector_slots_[start + member].guest != guest)) {
        compatible = false;
        break;
      }
      if (member != 0U &&
          resident_vector_hosts[start + member] !=
              resident_vector_hosts[start] + member) {
        compatible = false;
        break;
      }
    }
    if (!compatible)
      continue;
    for (unsigned member = 0; member < count; ++member)
      vector_slots_[start + member].guest = (first + member) & 31U;
    return true;
  }
  return false;
}

std::optional<std::size_t>
JitCompilerFamilies::allocate_vector_slot(unsigned guest) {
  if (!gpr_cache_enabled_)
    return std::nullopt;
  /*
   * Keep the unrestricted low-pressure shape stable by consuming q16-q31
   * first.  q6/q7 are prebound to constrained identities before emission;
   * any low slot left unbound after that planning pass is ordinary resident
   * capacity once the high bank is full.
   */
  for (std::size_t index = pinned_vector_slot_count;
       index < vector_slots_.size(); ++index) {
    auto &slot = vector_slots_[index];
    if (slot.guest != unbound_guest)
      continue;
    slot.guest = guest;
    return index;
  }
  for (std::size_t index = 0; index < pinned_vector_slot_count; ++index) {
    auto &slot = vector_slots_[index];
    if (slot.guest != unbound_guest)
      continue;
    slot.guest = guest;
    return index;
  }
  return std::nullopt;
}

std::size_t
JitCompilerFamilies::vector_slot_offset(unsigned guest) noexcept {
  return offsetof(arm64::CpuState, vectors) +
         guest * sizeof(arm64::VectorRegister);
}

JitCompilerFamilies::NativeVector
JitCompilerFamilies::read_native_vector(unsigned guest,
                                        unsigned fallback) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  auto slot = find_vector_slot(guest);
  if (!slot)
    slot = allocate_vector_slot(guest);
  const auto offset = vector_slot_offset(guest);
  if (slot) {
    auto &binding = vector_slots_[*slot];
    if (!binding.loaded) {
      emitter.load_q(resident_vector_hosts[*slot], 15, offset);
      binding.loaded = true;
    }
    return {resident_vector_hosts[*slot], true};
  }
  emitter.load_q(fallback, 15, offset);
  return {fallback, false};
}

JitCompilerFamilies::NativeVector
JitCompilerFamilies::read_constrained_native_vector(unsigned guest,
                                                    unsigned fallback) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  const auto offset =
      offsetof(arm64::CpuState, vectors) +
      guest * sizeof(arm64::VectorRegister);
  const auto slot = find_vector_slot(guest);
  if (slot && *slot < pinned_vector_slot_count) {
    auto &binding = vector_slots_[*slot];
    if (!binding.loaded) {
      emitter.load_q(resident_vector_hosts[*slot], 15, offset);
      binding.loaded = true;
    }
    return {resident_vector_hosts[*slot], true};
  }

  /*
   * A third constrained identity deliberately retains the established v2
   * fallback rather than shortening the block. load_q() is cache-aware, so
   * an earlier resident write is copied from its live host binding instead
   * of reloading stale CpuState.
   */
  load_q(fallback, 15, offset);
  return {fallback, false};
}

JitCompilerFamilies::NativeVector
JitCompilerFamilies::write_native_vector(unsigned guest, bool reads_old,
                                         unsigned fallback) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  auto slot = find_vector_slot(guest);
  if (!slot)
    slot = allocate_vector_slot(guest);
  const auto offset = vector_slot_offset(guest);
  if (slot) {
    auto &binding = vector_slots_[*slot];
    if (reads_old && !binding.loaded)
      emitter.load_q(resident_vector_hosts[*slot], 15, offset);
    binding.loaded = true;
    binding.dirty = true;
    return {resident_vector_hosts[*slot], true};
  }
  if (reads_old)
    emitter.load_q(fallback, 15, offset);
  return {fallback, false};
}

void JitCompilerFamilies::commit_native_vector(NativeVector value,
                                               unsigned guest) {
  if (value.resident)
    return;
  static_cast<Arm64Emitter *>(family_emitter_)
      ->store_q(value.host, 15, vector_slot_offset(guest));
}

void JitCompilerFamilies::emit_native_vector_unary(
    std::uint32_t instruction, bool reads_destination) {
  const unsigned destination = instruction & 31U;
  const unsigned source = (instruction >> 5) & 31U;
  const auto native_source = read_native_vector(source, 1U);
  const auto native_destination =
      write_native_vector(destination, reads_destination, 0U);
  emit((instruction & ~0x3ffU) | (native_source.host << 5) |
       native_destination.host);
  commit_native_vector(native_destination, destination);
}

void JitCompilerFamilies::emit_native_vector_binary(
    std::uint32_t instruction, bool reads_destination,
    BinaryFallbacks fallbacks) {
  const unsigned destination = instruction & 31U;
  const unsigned left = (instruction >> 5) & 31U;
  const unsigned right = (instruction >> 16) & 31U;
  const auto native_left = read_native_vector(left, fallbacks.left);
  const auto native_right = right == left
                                ? native_left
                                : read_native_vector(right, fallbacks.right);
  const auto native_destination = write_native_vector(
      destination, reads_destination, fallbacks.destination);
  emit((instruction & ~0x001f'03ffU) |
       (native_right.host << 16) | (native_left.host << 5) |
       native_destination.host);
  commit_native_vector(native_destination, destination);
}

void JitCompilerFamilies::emit_native_vector_by_element(
    std::uint32_t instruction) {
  const unsigned destination = instruction & 31U;
  const unsigned left = (instruction >> 5) & 31U;
  const unsigned right = (instruction >> 16) & 15U;
  const auto native_right = read_constrained_native_vector(right, 2U);
  const auto native_left =
      left == right ? native_right : read_native_vector(left, 1U);
  const auto native_destination =
      write_native_vector(destination, true, 0U);
  emit((instruction & ~0x000f'03ffU) |
       (native_right.host << 16) | (native_left.host << 5) |
       native_destination.host);
  commit_native_vector(native_destination, destination);
}

void JitCompilerFamilies::emit_native_vector_accumulate(
    std::uint32_t instruction, bool has_second) {
  const unsigned destination = instruction & 31U;
  const unsigned first = (instruction >> 5) & 31U;
  const auto native_destination =
      write_native_vector(destination, true, 0U);
  const auto native_first =
      first == destination ? native_destination
                           : read_native_vector(first, 1U);
  auto native = (instruction & ~0x3ffU) | (native_first.host << 5) |
                native_destination.host;
  if (has_second) {
    const unsigned second = (instruction >> 16) & 31U;
    const auto native_second =
        second == destination
            ? native_destination
            : second == first ? native_first
                              : read_native_vector(second, 2U);
    native = (native & ~(31U << 16)) | (native_second.host << 16);
  }
  emit(native);
  commit_native_vector(native_destination, destination);
}

void JitCompilerFamilies::load_gpr(unsigned host, unsigned guest,
                                   bool stack_pointer) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  if (guest == 31U && !stack_pointer) {
    emitter.move_x(host, 31);
    return;
  }
  const auto slot = find_gpr_slot(guest, stack_pointer);
  if (slot) {
    if (!gpr_slots_[*slot].loaded) {
      emitter.load_x(resident_gpr_hosts[*slot], 15,
                     register_offset(guest, stack_pointer));
      gpr_slots_[*slot].loaded = true;
    }
    if (resident_gpr_hosts[*slot] != host)
      emitter.move_x(host, resident_gpr_hosts[*slot]);
    return;
  }
  emitter.load_x(host, 15, register_offset(guest, stack_pointer));
}

void JitCompilerFamilies::store_gpr(unsigned host, unsigned guest,
                                    bool stack_pointer) {
  if (guest == 31U && !stack_pointer)
    return;
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  const auto slot = find_gpr_slot(guest, stack_pointer);
  if (slot) {
    if (resident_gpr_hosts[*slot] != host)
      emitter.move_x(resident_gpr_hosts[*slot], host);
    gpr_slots_[*slot].loaded = true;
    gpr_slots_[*slot].dirty = true;
    return;
  }
  emitter.store_x(host, 15, register_offset(guest, stack_pointer));
}

std::uint16_t JitCompilerFamilies::dirty_gpr_mask() const noexcept {
  std::uint16_t mask = 0;
  for (std::size_t index = 0; index < gpr_slots_.size(); ++index)
    /* Kept across links, every binding is as new as the last block's: the
     * memory behind it is not current. */
    if (resident_across_links_ ? gpr_slots_[index].guest != unbound_guest
                               : gpr_slots_[index].dirty)
      mask |= static_cast<std::uint16_t>(1U << index);
  return mask;
}

std::uint32_t JitCompilerFamilies::dirty_vector_mask() const noexcept {
  std::uint32_t mask = 0;
  for (std::size_t index = 0; index < vector_slots_.size(); ++index)
    if (vector_slots_[index].dirty)
      mask |= std::uint32_t{1} << index;
  return mask;
}

void JitCompilerFamilies::flush_gprs(std::uint16_t mask) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  for (std::size_t index = 0; index < gpr_slots_.size(); ++index) {
    const auto &slot = gpr_slots_[index];
    if ((mask & (1U << index)) == 0U || slot.guest == unbound_guest)
      continue;
    if (resident_across_links_ && index + 1U < gpr_slots_.size()) {
      /* Two bindings of neighbouring guest registers are one store. */
      const auto &next = gpr_slots_[index + 1U];
      if ((mask & (2U << index)) != 0U && next.guest == slot.guest + 1U &&
          !slot.stack_pointer && !next.stack_pointer) {
        emitter.store_pair_x(resident_gpr_hosts[index],
                             resident_gpr_hosts[index + 1U], 15,
                             register_offset(slot.guest, false));
        ++index;
        continue;
      }
    }
    emitter.store_x(resident_gpr_hosts[index], 15,
                    register_offset(slot.guest, slot.stack_pointer));
  }
}

void JitCompilerFamilies::flush_vectors(std::uint32_t mask) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  for (std::size_t index = 0; index < vector_slots_.size(); ++index) {
    const auto &slot = vector_slots_[index];
    if ((mask & (1U << index)) == 0U || slot.guest == unbound_guest)
      continue;
    emitter.store_q(resident_vector_hosts[index], 15,
                    vector_slot_offset(slot.guest));
  }
}

void JitCompilerFamilies::set_exit_gpr_mask(std::uint16_t mask) noexcept {
  exit_gpr_mask_ = mask;
  exit_gpr_mask_set_ = true;
}

void JitCompilerFamilies::set_exit_vector_mask(
    std::uint32_t mask) noexcept {
  exit_vector_mask_ = mask;
  exit_vector_mask_set_ = true;
}

void JitCompilerFamilies::flush_exit_gprs() {
  const auto gpr_mask =
      exit_gpr_mask_set_ ? exit_gpr_mask_ : dirty_gpr_mask();
  const auto vector_mask =
      exit_vector_mask_set_ ? exit_vector_mask_ : dirty_vector_mask();
  flush_gprs(gpr_mask);
  flush_vectors(vector_mask);
  /*
   * Exit code can be emitted on one arm of a generated branch.  Publish a
   * dirty host NZCV for that arm, but deliberately retain the translation
   * state: the other arm did not execute this store and may continue using
   * the live guest flags.
  */
  flush_flags_for_exit();
  exit_gpr_mask_set_ = false;
  exit_vector_mask_set_ = false;
}

void JitCompilerFamilies::flush_link_transition_state() {
  /* The successor takes the core registers from the hosts. */
  const std::uint16_t gpr_mask =
      resident_across_links_ ? 0U
      : exit_gpr_mask_set_   ? exit_gpr_mask_
                             : dirty_gpr_mask();
  const auto vector_mask =
      exit_vector_mask_set_ ? exit_vector_mask_ : dirty_vector_mask();
  flush_gprs(gpr_mask);
  flush_vectors(vector_mask);
  flush_flags_for_exit();
  exit_gpr_mask_set_ = false;
  exit_vector_mask_set_ = false;
}

void JitCompilerFamilies::spill_gprs_for_terminal_call() {
  const auto gpr_mask =
      exit_gpr_mask_set_ ? exit_gpr_mask_ : dirty_gpr_mask();
  const auto vector_mask =
      exit_vector_mask_set_ ? exit_vector_mask_ : dirty_vector_mask();
  flush_gprs(gpr_mask);
  flush_vectors(vector_mask);
  spill_flags_for_internal_clobber();
  /*
   * The return emitter still invokes the centralized exit hook. Leave an
   * explicit empty mask so it cannot flush bindings allocated on a later hot
   * path into an earlier cold exit.
   */
  exit_gpr_mask_ = 0;
  exit_gpr_mask_set_ = true;
  exit_vector_mask_ = 0;
  exit_vector_mask_set_ = true;
}

void JitCompilerFamilies::store_pc(std::uint64_t pc) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  emitter.load_literal_constant(8, pc);
  emitter.store_x(8, 15, offsetof(arm64::CpuState, pc));
}

void JitCompilerFamilies::load_flags() {
  if (guest_flags_state_ != GuestFlagsState::memory)
    return;
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  emitter.load_w(11, 15, offsetof(arm64::CpuState, nzcv));
  emitter.emit(0xd51b'420bU); // msr nzcv, x11
  guest_flags_state_ = GuestFlagsState::synchronized;
}

void JitCompilerFamilies::store_flags() {
  /* A native guest instruction has just produced the complete guest NZCV. */
  guest_flags_state_ = GuestFlagsState::host_dirty;
}

void JitCompilerFamilies::flush_flags_for_exit() {
  if (guest_flags_state_ != GuestFlagsState::host_dirty)
    return;
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  emitter.emit(0xd53b'420bU); // mrs x11, nzcv
  emitter.store_w(11, 15, offsetof(arm64::CpuState, nzcv));
}

void JitCompilerFamilies::synchronize_flags() {
  flush_flags_for_exit();
  if (guest_flags_state_ == GuestFlagsState::host_dirty)
    guest_flags_state_ = GuestFlagsState::synchronized;
}

void JitCompilerFamilies::spill_flags_for_internal_clobber() {
  flush_flags_for_exit();
  guest_flags_state_ = GuestFlagsState::memory;
}

void JitCompilerFamilies::assume_flags_in_memory() noexcept {
  guest_flags_state_ = GuestFlagsState::memory;
}

JitCompilerFamilies::GuestFlagsState
JitCompilerFamilies::snapshot_flags() const noexcept {
  return guest_flags_state_;
}

void JitCompilerFamilies::publish_cold_flags(GuestFlagsState state) {
  if (state != GuestFlagsState::host_dirty)
    return;
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  emitter.emit(0xd53b'420bU); // mrs x11, nzcv
  emitter.store_w(11, 15, offsetof(arm64::CpuState, nzcv));
}

void JitCompilerFamilies::read_flags(unsigned destination) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  if (guest_flags_state_ == GuestFlagsState::memory) {
    emitter.load_w(destination, 15, offsetof(arm64::CpuState, nzcv));
    return;
  }
  emitter.emit(0xd53b'4200U | (destination & 31U)); // mrs Xd, nzcv
}

void JitCompilerFamilies::mark_native_fp() noexcept {
  *family_native_fp_ = true;
}

void JitCompilerFamilies::emit_linked_exit(std::uint64_t target,
                                           std::size_t count) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  flush_link_transition_state();
  /*
   * Unlike an exit emitted on only one branch arm, every path entering this
   * linked-exit sequence publishes the ordinary resident state above.
   */
  assume_flags_in_memory();
  /* Where the registers stay in the host, the paths that return to the
   * executor write them back all. */
  set_exit_gpr_mask(resident_across_links_ ? dirty_gpr_mask() : 0);
  set_exit_vector_mask(0);
  /* The successor runs in this translation's mode: nothing in a block
   * changes SVCR or top-byte-ignore and then reaches a linked exit. */
  const auto key = translation_key(target, translation_mode());
  const auto *link =
      link_resolver_ == nullptr
          ? nullptr
          : static_cast<const JitBlockLink *>(
                link_resolver_(link_context_, key));
  if (link == nullptr) {
    /* No resolver deliberately selects fully unlinked code. A configured
     * resolver returning null means its stable table refused this target,
     * currently because that table reached capacity. The fallback remains
     * correct executor-private code, but it is observably degraded and must
     * not become the canonical shared translation for these guest bytes.
     */
    direct_link_refusals_ += link_resolver_ != nullptr;
    store_pc(target);
    emitter.return_constant(Event::continued, count);
    return;
  }

  const auto fallback = emitter.make_label();
  const unsigned record = link_pointer_;
  const unsigned scratch = link_scratch_;
  emitter.load_relocatable_pointer(
      record, reinterpret_cast<std::uintptr_t>(link),
      JitRelocationKind::direct_link, key);
  /*
   * Subtract the completed source while the descriptor pointer load is in
   * flight. Both a missing capability and a full fairness window take the
   * shared cold undo below, so only successful links commit the decrement.
   * x21 is initialized once from the host-owned capability selector; guest
   * data never contributes to the indexed native branch target.
   */
  emitter.subtract_immediate(20, 20, static_cast<unsigned>(count));
  emitter.load_pair_x(16, 9, record, 0);
  emitter.load_w(scratch, record, offsetof(JitBlockLink, allowed_selectors));
  emitter.logical_shift_right_variable(scratch, scratch, 21);
  emitter.test_zero(fallback, scratch, 0, false);
  emitter.load_x(scratch, record, offsetof(JitBlockLink, internal_entry));
  emitter.compare_zero(fallback, scratch, true, false);

  emitter.compare_register(20, 9);
  emitter.branch_condition(fallback, 3); // LO

  /*
   * x15/x14 still hold CpuState/TLB, and the outer frame owns all chain
   * state. Enter the successor after its external prologue so a successful
   * link executes no argument reload and creates no extra native frame.
   * Every successor exit therefore returns through the one frame established
   * by the initial C++ entry.
   */
  emitter.branch_register(scratch);

  emitter.bind(fallback);
  emitter.add_immediate(20, 20, static_cast<unsigned>(count));
  store_pc(target);
  emitter.return_constant(Event::continued, count);
}

void JitCompilerFamilies::emit_indirect_linked_exit(
    unsigned target_host, bool writes_link, std::uint64_t source_pc,
    std::size_t count, bool target_table) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  /*
   * Preserve the architectural target before BLR can replace a resident X30
   * binding. x8 is executor-owned scratch and is never a native branch
   * operand on this path.
   */
  if (target_host != 8U)
    emitter.move_x(8, target_host);
  if (writes_link) {
    const auto link_register =
        write_native_gpr(30, false, false, 9);
    emitter.load_literal_constant(link_register.host,
                                  source_pc + sizeof(std::uint32_t));
    commit_native_gpr(link_register, 30, false);
  }

  flush_link_transition_state();
  /*
   * Ordinary CpuState and NZCV are canonical before host-only comparisons.
   */
  assume_flags_in_memory();
  set_exit_gpr_mask(resident_across_links_ ? dirty_gpr_mask() : 0);
  set_exit_vector_mask(0);

  /* The site and every target it trains belong to this translation's mode;
   * its entries compare plain guest PCs with x8. */
  const auto source_key = translation_key(source_pc, translation_mode());
  const auto *site =
      indirect_link_resolver_ == nullptr
          ? nullptr
          : static_cast<const JitIndirectLink *>(
                indirect_link_resolver_(indirect_link_context_, source_key));
  if (site == nullptr) {
    indirect_link_refusals_ += indirect_link_resolver_ != nullptr;
    emitter.store_x(8, 15, offsetof(CpuState, pc));
    emitter.return_constant(Event::continued, count);
    return;
  }

  const auto matched = emitter.make_label();
  const auto miss = emitter.make_label();
  const auto fallback = emitter.make_label();
  const auto undo_and_return_to_cpp = emitter.make_label();
  /* The site is in x13 (x11 where the registers stay in the host), the
   * matched record in x12 (x10). */
  const unsigned record = link_pointer_;
  const unsigned site_register = link_scratch_;
  emitter.load_relocatable_pointer(
      site_register, reinterpret_cast<std::uintptr_t>(site),
      JitRelocationKind::indirect_link, source_key);
  /*
   * Hide the independent budget update under the dependent PIC descriptor
   * load. Every miss or rejected matched entry rejoins at the one cold undo.
   */
  emitter.subtract_immediate(20, 20, static_cast<unsigned>(count));
  const auto load_entry = [&](std::size_t offset) {
    /* ldp x9, x12, [x13, #offset]: {guest target, host entry table}. */
    emitter.load_pair_x(9, record, site_register,
                        static_cast<std::ptrdiff_t>(offset));
  };
  load_entry(offsetof(JitIndirectLink, entries));
  emitter.compare_register(8, 9);
  emitter.branch_condition(matched, 0); // EQ
  load_entry(offsetof(JitIndirectLink, entries) +
             sizeof(JitIndirectLinkEntry));
  emitter.compare_register(8, 9);
  if (target_table) {
    emitter.branch_condition(matched, 0); // EQ
    /* Neither cell holds the target: its entry in the executor's table, if
     * it was trained in this context, names the record x12. x13 stays the
     * site for the miss. */
    emitter.load_x(16, 19, offsetof(JitChainState, indirect_targets));
    emitter.load_x(9, 19, offsetof(JitChainState, indirect_target_tag));
    // eor x12, x8, x8, lsr #14: the index of jit_indirect_target_index().
    emitter.emit(0xca40'0000U | (14U << 10) | (8U << 16) | (8U << 5) | record);
    emitter.ubfm(record, record, 2, 1U + jit_indirect_target_bits);
    emitter.add_shifted(record, 16, record, 4);
    emitter.emit(0xaa00'0000U | (8U << 16) | (9U << 5) | 9U); // orr x9, x9, x8
    emitter.load_pair_x(16, record, record, 0);
    emitter.compare_register(9, 16);
    emitter.branch_condition(miss, 1); // NE
  } else {
    emitter.branch_condition(miss, 1); // NE
  }
  emitter.bind(matched);

  /*
   * x12 is a stable host record selected by a guest-target equality test.
   * Only its host-published internal entry can reach BR below.
   */
  emitter.load_pair_x(16, 9, record, 0);
  emitter.load_w(site_register, record,
                 offsetof(JitBlockLink, allowed_selectors));
  emitter.logical_shift_right_variable(site_register, site_register, 21);
  emitter.test_zero(fallback, site_register, 0, false);
  emitter.load_x(site_register, record, offsetof(JitBlockLink, internal_entry));
  emitter.compare_zero(fallback, site_register, true, false);

  emitter.compare_register(20, 9);
  emitter.branch_condition(fallback, 3); // LO

  emitter.branch_register(site_register);

  emitter.bind(miss);
  emitter.store_x(8, 15, offsetof(CpuState, pc));
  emitter.store_x(site_register, 19, offsetof(JitChainState, indirect_miss));
  emitter.load_literal_constant(9, source_key);
  emitter.store_x(9, 19, offsetof(JitChainState, indirect_source_pc));
  emitter.branch(undo_and_return_to_cpp);

  emitter.bind(fallback);
  /* x8 still holds the target PC; the matched record's key in x16 equals it
   * only in mode 0, and a top-byte-ignore translation's key has a state bit
   * beside the PC. */
  emitter.store_x(8, 15, offsetof(CpuState, pc));
  emitter.bind(undo_and_return_to_cpp);
  emitter.add_immediate(20, 20, static_cast<unsigned>(count));
  emitter.return_constant(Event::continued, count);
}

} // namespace uml::cpu::arm64::jit
