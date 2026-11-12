// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#if UML_RUN_ROUTING_TESTING
namespace uml::routing::testing {

enum class TransactionBoundary {
	commit,
	retirement,
};

/* Available only from the separately compiled unit-test variant. The next
 * selected transaction pauses after its state mutation while still holding
 * the registry lock. An unbound resolver announces its attempt and releases
 * the transaction, then observes the complete post-transaction snapshot. */
void pause_next_transaction(TransactionBoundary boundary) noexcept;
[[nodiscard]] bool wait_for_transaction_pause(
	unsigned int timeout_milliseconds) noexcept;
void cancel_transaction_pause() noexcept;

} // namespace uml::routing::testing
#endif
