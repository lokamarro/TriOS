// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/a32_encodings.hpp"

#include <cstdint>

namespace uml::cpu::arm64::aarch32 {

/*
 * Library-exit counters: with counting on, every execute_a32() call adds to
 * the count of the form it executed, so a test can require a form an engine
 * lowers to stay out of the library from a clean state.
 *
 * They are a test hook that only the semantic library's unit-test variant
 * has (uml_arm64_aarch32_semantics_exit_counting, built with
 * UML_A32_LIBRARY_EXIT_COUNTERS): the tests that read them link that variant,
 * and the production library keeps no counter.
 */
void count_a32_library_exits(bool enable) noexcept;
[[nodiscard]] std::uint64_t a32_library_exits(Form form) noexcept;
[[nodiscard]] std::uint64_t a32_library_exit_total() noexcept;
void reset_a32_library_exits() noexcept;

namespace detail {

/* What execute_a32() does at its entry in the variant. */
void note_library_exit(Form form) noexcept;

} // namespace detail

} // namespace uml::cpu::arm64::aarch32
