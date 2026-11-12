// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace uml::runtime {

/* Whether a paused guest's RAM is hibernated (guest_hibernation.hpp). */
enum class HibernationMode {
	/* Where guest RAM is wired, that is on a physical iOS device. */
	automatic,
	off,
	/* Everywhere, which is how tests and diagnosis reach the same code on a
	 * host that does not wire. */
	on,
};

/* Whether a guest whose RAM is (`wires`) or is not wired hibernates under
 * `mode`. RAM the system can already compress gains nothing from a file. */
[[nodiscard]] constexpr bool hibernation_applies(HibernationMode mode,
	bool wires) noexcept
{
	return mode == HibernationMode::on ||
		(mode == HibernationMode::automatic && wires);
}

} // namespace uml::runtime
