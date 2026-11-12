// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "require.hpp"

#include <cstdio>

namespace {

using uml::testing::require;

} // namespace

int main() {
  namespace testing = uml::cpu::arm64::jit::testing;
  require(testing::exclusive_operations_are_serialized_for_testing(),
          "exclusive helpers entered the serialized phase concurrently");
  require(testing::normal_store_leaves_exclusive_gate_unchanged_for_testing(),
          "an uncontended store mutated the helper phase");
  require(testing::store_first_publication_handshake_for_testing(),
          "exclusive helper crossed an already-published native store");
  require(testing::waiting_store_precedes_later_helper_for_testing(),
          "a later helper crossed a previously waiting store");
  require(testing::waiting_store_precedes_already_queued_helper_for_testing(),
          "an already-queued helper did not yield to a waiting store");
  require(testing::all_waiting_stores_precede_queued_helper_for_testing(),
          "a queued helper crossed one of multiple waiting stores");
  require(testing::late_store_waits_for_current_helper_for_testing(),
          "a late store displaced the helper which owned the current phase");
  require(
      testing::tracked_store_invalidation_does_not_deadlock_for_testing(),
      "a helper blocked tracked-store invalidation while scanning publications");
  require(testing::publication_gate_randomized_stress_for_testing(),
          "randomized helper/store stress violated publication exclusion");
  require(testing::publication_participant_lifetime_is_idempotent_for_testing(),
          "participant registration left a duplicate or stale scan pointer");
  std::puts("arm64-jit-publication-gate: concurrency checks passed");
  return 0;
}
