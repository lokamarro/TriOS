// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform/apple/host_features.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace {

namespace jit_test = uml::cpu::arm64::jit::testing;

constexpr std::uint32_t dsb_base = 0xd503'309fU;
constexpr std::uint32_t dmb_base = 0xd503'30bfU;
constexpr std::uint32_t isb_base = 0xd503'30dfU;
constexpr std::uint32_t dsb_nxs_base = 0xd503'323fU;
constexpr std::uint32_t sb = 0xd503'30ffU;
constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

using uml::testing::require;

[[nodiscard]] constexpr std::uint32_t standard(std::uint32_t base,
                                               unsigned option) noexcept {
  return base | ((option & 15U) << 8U);
}

[[nodiscard]] constexpr std::uint32_t nxs(unsigned scope) noexcept {
  return dsb_nxs_base | ((scope & 3U) << 10U);
}

template <std::size_t Size>
void require_words(const std::vector<std::uint32_t> &actual,
                   const std::array<std::uint32_t, Size> &expected,
                   const char *message) {
  require(actual.size() == expected.size() &&
              std::equal(actual.begin(), actual.end(), expected.begin()),
          message);
}

void forced_feature_selection() {
  for (const auto base : {dmb_base, dsb_base, isb_base})
    for (unsigned option = 0; option < 16U; ++option) {
      const auto instruction = standard(base, option);
      require_words(
          jit_test::lower_memory_barrier_for_testing(instruction, false),
          std::array{instruction},
          "baseline barrier changed under the forced feature-off selector");
    }

  require_words(jit_test::lower_memory_barrier_for_testing(sb, true),
                std::array{sb},
                "native speculation barrier was not emitted one-for-one");
  require_words(jit_test::lower_memory_barrier_for_testing(sb, false),
                std::array{standard(dsb_base, 15U), standard(isb_base, 15U)},
                "portable speculation barrier substitution is incorrect");
}

/* The virtual CPU implements no FEAT_XS: DSB nXS compiles to the same
 * illegal terminal as an unknown word. */
void restricted_completion_barrier_is_absent() {
  constexpr std::uint64_t pc = 0x4f81'0000;
  const std::array unknown{0xffff'ffffU};
  const auto reference = jit_test::compile_block(pc, unknown);
  for (unsigned scope = 0; scope < 4U; ++scope) {
    const std::array guest{nxs(scope), terminal_system_call};
    const auto compiled = jit_test::compile_block(pc, guest);
    require(compiled.guest_instructions == 1U &&
                compiled.words == reference.words,
            "DSB nXS did not compile to the illegal terminal");
  }
}

void current_host_compiler_selection() {
  constexpr std::uint64_t pc = 0x4f80'0000;
  const std::array guest{sb, terminal_system_call};
  const auto compiled = jit_test::compile_block(pc, guest);
  require(!compiled.guest_illegal,
          "allocated barrier sequence compiled as guest illegal");
  require(compiled.guest_instructions == guest.size(),
          "barrier sequence did not remain in one translated block");
  require(compiled.hot_words <= compiled.words.size(),
          "barrier compiler returned an invalid hot-code extent");
  const std::span<const std::uint32_t> hot{compiled.words.data(),
                                           compiled.hot_words};

  const auto &features = ::uml::platform::apple::probed_host_capabilities();
  const auto selected_sb = jit_test::lower_memory_barrier_for_testing(
      sb, features.has(::uml::cpu::ArmFeature::sb));
  require(std::search(hot.begin(), hot.end(), selected_sb.begin(),
                      selected_sb.end()) != hot.end(),
          "current-host speculation barrier lowering is absent from hot code");
  if (!features.has(::uml::cpu::ArmFeature::sb))
    require(std::find(hot.begin(), hot.end(), sb) == hot.end(),
            "unsupported speculation barrier remained in hot code");
}

} // namespace

int main() {
  forced_feature_selection();
  restricted_completion_barrier_is_absent();
  current_host_compiler_selection();
  std::puts("arm64-jit-barrier-codegen: translation selection passed");
  return 0;
}
