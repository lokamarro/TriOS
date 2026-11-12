// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::platform {

/* Whether this process is an iOS application running on macOS.
 *
 * An iOS build in Designed for iPad mode keeps the iOS SDK, the iOS sandbox,
 * and the iOS code-signing rules, but it runs inside a macOS process and
 * therefore inherits the macOS address-space layout rather than a device's.
 * Anything that places host mappings at a fixed address has to ask.
 */
[[nodiscard]] bool running_ios_app_on_macos() noexcept;

/* The environment variable that states the process's memory limit, in whole
 * mebibytes, for a host that sets none. */
inline constexpr const char *memory_limit_variable = "UML_MEMORY_LIMIT_MIB";

/* What a process's limit is when the host sets `os_limit` (zero for none) and
 * a launch states `configured` (zero for none): the host's own figure wins, so
 * a stated limit never loosens or tightens what iOS enforces. */
[[nodiscard]] constexpr std::uint64_t resolve_process_memory_limit(
	std::uint64_t os_limit, std::uint64_t configured) noexcept
{
	return os_limit != 0 ? os_limit : configured;
}

/* The mebibytes `text` names as bytes: a plain decimal number, nothing else,
 * of at least 1 and at most 1048576 (1 TiB). Zero when `text` is null, empty
 * or anything but that. */
[[nodiscard]] std::uint64_t parse_memory_limit_mib(const char *text) noexcept;

/* The limit a launch states for a host that sets none: the value given to
 * set_configured_process_memory_limit (the launch option), else the one
 * memory_limit_variable names, else zero. A variable that is set to anything
 * but a whole number of mebibytes is reported once on stderr and ignored. */
[[nodiscard]] std::uint64_t configured_process_memory_limit_bytes() noexcept;

/* Gives the limit the launch option states; zero withdraws it, so the variable
 * applies again. */
void set_configured_process_memory_limit(std::uint64_t bytes) noexcept;

/* Whether the process's limit is one a launch stated rather than one the host
 * enforces, which is when nothing ends the process at it. */
[[nodiscard]] bool process_memory_limit_is_virtual() noexcept;

/* The memory footprint the host lets this process reach before it ends it:
 * iOS sets one per application (jetsam). macOS sets none for an ordinary
 * process, so there the limit a launch stated stands in for it
 * (configured_process_memory_limit_bytes), which lets a measurement run on a
 * Mac see the budgets that derive from the limit as the device does. Zero when
 * neither exists. One task_info call. */
[[nodiscard]] std::uint64_t process_memory_limit_bytes() noexcept;

} // namespace uml::platform
