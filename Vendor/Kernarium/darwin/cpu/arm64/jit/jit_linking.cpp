// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_linking.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>

namespace uml::cpu::arm64 {

namespace {

constexpr std::size_t allocation_allowance = 64U;

[[nodiscard]] std::size_t index_capacity_for(std::size_t capacity) noexcept {
  if (capacity == 0)
    return 0;
  if (capacity > std::numeric_limits<std::uint32_t>::max() - 1U ||
      capacity > std::numeric_limits<std::size_t>::max() / 2U)
    return 0;
  return std::bit_ceil(capacity * 2U);
}

[[nodiscard]] std::size_t bucket_capacity_for(
    std::size_t capacity) noexcept {
  if (capacity == 0)
    return 0;
  if (capacity > std::numeric_limits<std::size_t>::max() / 2U)
    return 0;
  return std::bit_ceil(capacity);
}

[[nodiscard]] std::size_t hash_link_key(std::uint64_t key,
                                        std::size_t mask) noexcept {
  key *= 0x9e37'79b9'7f4a'7c15ULL;
  key ^= key >> 29U;
  return static_cast<std::size_t>(key) & mask;
}

template <typename Record>
[[nodiscard]] std::size_t fixed_table_allocation_bytes(
    std::size_t capacity) noexcept {
  const auto index_capacity = index_capacity_for(capacity);
  if (capacity == 0 || index_capacity == 0 ||
      capacity > (std::numeric_limits<std::size_t>::max() -
                  2U * allocation_allowance) /
                     sizeof(Record))
    return 0;
  const auto records = capacity * sizeof(Record);
  if (index_capacity >
      (std::numeric_limits<std::size_t>::max() - records -
       2U * allocation_allowance) /
          sizeof(std::uint32_t))
    return 0;
  return records + index_capacity * sizeof(std::uint32_t) +
         2U * allocation_allowance;
}

template <typename Record>
[[nodiscard]] Record *allocate_records(std::size_t capacity) {
  if (capacity == 0)
    return nullptr;
  return static_cast<Record *>(
      ::operator new(capacity * sizeof(Record),
                     std::align_val_t{alignof(Record)}));
}

template <typename Record>
void destroy_records(Record *records, std::size_t size) noexcept {
  if (records != nullptr)
    std::destroy_n(records, size);
}

template <typename Record>
void deallocate_records(Record *records) noexcept {
  if (records != nullptr)
    ::operator delete(records, std::align_val_t{alignof(Record)});
}

} // namespace

std::uint8_t checked_link_selector(std::uint8_t capabilities) noexcept {
  const auto selector = link_selector_for_capabilities(capabilities);
  /* Selector zero carries no setup capabilities. Every successor entry whose
   * requirements are not met is already null, so it safely exits to C++.
   * This also prevents an invalid host-owned mask from becoming an out-of-
   * bounds generated-code table read. */
  return selector == invalid_link_selector ? 0U : selector;
}

JitBlockLinkTable::JitBlockLinkTable(std::size_t capacity)
    : capacity_{capacity}, index_capacity_{bucket_capacity_for(capacity)} {
  if (capacity_ != 0 && allocation_bytes(capacity_) == 0)
    throw std::length_error{"JIT block-link table is too large"};
  if (index_capacity_ != 0) {
    index_ =
        std::make_unique_for_overwrite<JitBlockLink *[]>(index_capacity_);
    std::fill_n(index_.get(), index_capacity_, nullptr);
    records_ = allocate_records<JitBlockLink>(capacity_);
  }
}

JitBlockLinkTable::~JitBlockLinkTable() {
  destroy_records(records_, size_);
  deallocate_records(records_);
}

/* Publishing the bucket before the record's link is safe because one
 * executor is used by only one host thread at a time. */
JitBlockLink *JitBlockLinkTable::find_or_create(
    std::uint64_t pc, unsigned long generation) noexcept {
  if (index_capacity_ == 0)
    return nullptr;
  const auto slot = hash_link_key(pc ^ generation, index_capacity_ - 1U);
  auto *previous = index_[slot];
  for (auto *link = previous; link != nullptr; link = link->next_target)
    if (link->guest_pc == pc && link->generation == generation)
      return link;
  if (size_ >= capacity_)
    return nullptr;
  auto *record = std::construct_at(
      records_ + size_,
      JitBlockLink{.guest_pc = pc, .generation = generation});
  index_[slot] = record;
  ++size_;
  record->next_target = previous;
  return record;
}

JitBlockLink *JitBlockLinkTable::find(
    std::uint64_t pc, unsigned long generation) const noexcept {
  if (index_capacity_ == 0)
    return nullptr;
  for (auto *link = index_[hash_link_key(pc ^ generation, index_capacity_ - 1U)];
       link != nullptr; link = link->next_target)
    if (link->guest_pc == pc && link->generation == generation)
      return link;
  return nullptr;
}

void JitBlockLinkTable::clear() noexcept {
  destroy_records(records_, size_);
  size_ = 0;
  if (index_ != nullptr)
    std::fill_n(index_.get(), index_capacity_, nullptr);
}

std::size_t JitBlockLinkTable::size() const noexcept {
  return size_;
}

std::size_t JitBlockLinkTable::allocation_bytes(
    std::size_t capacity) noexcept {
  const auto buckets = bucket_capacity_for(capacity);
  if (capacity == 0 || buckets == 0 ||
      buckets > (std::numeric_limits<std::size_t>::max() -
                 2U * allocation_allowance) /
                    sizeof(JitBlockLink *))
    return 0;
  const auto index_bytes = buckets * sizeof(JitBlockLink *);
  if (capacity > (std::numeric_limits<std::size_t>::max() - index_bytes -
                  2U * allocation_allowance) / sizeof(JitBlockLink))
    return 0;
  return index_bytes + capacity * sizeof(JitBlockLink) +
         2U * allocation_allowance;
}

JitIndirectLinkTable::JitIndirectLinkTable(
    std::size_t capacity, const JitBlockLink *empty_link)
    : capacity_{capacity}, index_capacity_{index_capacity_for(capacity)},
      empty_link_{empty_link} {
  if (capacity_ != 0 && allocation_bytes(capacity_) == 0)
    throw std::length_error{"JIT indirect-link table is too large"};
  if (index_capacity_ != 0) {
    index_ = std::make_unique_for_overwrite<std::uint32_t[]>(index_capacity_);
    std::fill_n(index_.get(), index_capacity_, std::uint32_t{0});
    records_ = allocate_records<JitIndirectLink>(capacity_);
  }
}

JitIndirectLinkTable::~JitIndirectLinkTable() {
  destroy_records(records_, size_);
  deallocate_records(records_);
}

JitIndirectLink *
JitIndirectLinkTable::find_or_create(
    std::uint64_t source_pc, unsigned long generation) {
  if (index_capacity_ == 0)
    return nullptr;
  const auto mask = index_capacity_ - 1U;
  auto slot = hash_link_key(source_pc ^ generation, mask);
  while (index_[slot] != 0) {
    auto &record = records_[index_[slot] - 1U];
    if (record.source_pc == source_pc && record.generation == generation)
      return &record;
    slot = (slot + 1U) & mask;
  }
  if (size_ >= capacity_)
    return nullptr;
  auto *result = std::construct_at(records_ + size_, JitIndirectLink{});
  reset(*result);
  result->source_pc = source_pc;
  result->generation = generation;
  index_[slot] = static_cast<std::uint32_t>(++size_);
  return result;
}

JitIndirectLink *
JitIndirectLinkTable::find(
    std::uint64_t source_pc, unsigned long generation) const noexcept {
  if (index_capacity_ == 0)
    return nullptr;
  const auto mask = index_capacity_ - 1U;
  auto slot = hash_link_key(source_pc ^ generation, mask);
  while (index_[slot] != 0) {
    auto &record = records_[index_[slot] - 1U];
    if (record.source_pc == source_pc && record.generation == generation)
      return &record;
    slot = (slot + 1U) & mask;
  }
  return nullptr;
}

void JitIndirectLinkTable::reset(JitIndirectLink &site) const noexcept {
  for (auto &entry : site.entries) {
    entry.guest_pc = 1;
    entry.link = empty_link_;
  }
  site.next_replacement = 0;
  site.activation_epoch = 0;
}

void JitIndirectLinkTable::clear() noexcept {
  destroy_records(records_, size_);
  size_ = 0;
  if (index_ != nullptr)
    std::fill_n(index_.get(), index_capacity_, std::uint32_t{0});
}

std::size_t JitIndirectLinkTable::size() const noexcept {
  return size_;
}

std::size_t JitIndirectLinkTable::allocation_bytes(
    std::size_t capacity) noexcept {
  return fixed_table_allocation_bytes<JitIndirectLink>(capacity);
}

} // namespace uml::cpu::arm64
