/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace uml::emulator {

/*
 * The loop guard of an application that boots a guest again after the guest
 * restarted itself.
 *
 * A restart is early when it comes less than `early_restart_window` after its
 * boot started. A guest that restarts early `early_restart_limit` times in a
 * row is not going to boot: the application ends the loop instead of
 * rebooting it for ever. A boot that lasts `early_restart_window` or more
 * resets the count. These constants are the one place the policy lives; every
 * application that runs the loop uses them.
 */
inline constexpr std::chrono::seconds early_restart_window{15};
inline constexpr unsigned early_restart_limit = 5;

/* The macOS launchers' exit status when the guard ended the loop. 0 is a
 * power-off and 1 a launch error; a guest never ends with 3. */
inline constexpr int early_restart_exit_status = 3;

/* The library refuses to hand a restarted guest back, with
 * -UML_EMULATOR_RUNTIME_ERROR and no side effect, while a kernel thread of
 * the old guest is still ending. The refusal can be retried: an application
 * runs again up to this many times, this far apart, before it ends the launch
 * on the refusal. */
inline constexpr unsigned restart_run_retries = 3;
inline constexpr std::chrono::milliseconds restart_run_retry_delay{500};

/* The restart command is the guest's raw bytes, control characters and
 * quotes included. This is what an application may log or show: printable
 * ASCII as it is, a backslash or a single quote with a backslash before it,
 * and any other byte as \xNN, so the log line it sits in cannot be broken or
 * made to look like another. */
[[nodiscard]] inline std::string printable_restart_command(
	std::string_view command)
{
	constexpr char digits[] = "0123456789abcdef";
	std::string printable;
	printable.reserve(command.size());
	for (const char character : command) {
		const auto byte = static_cast<unsigned char>(character);
		if (byte == '\\' || byte == '\'') {
			printable.push_back('\\');
			printable.push_back(character);
		} else if (byte >= 0x20 && byte < 0x7f) {
			printable.push_back(character);
		} else {
			printable += "\\x";
			printable.push_back(digits[byte >> 4]);
			printable.push_back(digits[byte & 0xf]);
		}
	}
	return printable;
}

class RestartLoopGuard final {
public:
	using Clock = std::chrono::steady_clock;

	/* A boot starts at `now`. */
	void boot_started(Clock::time_point now) noexcept { started_ = now; }

	/* The guest asked to restart at `now`. Whether the application may boot
	 * it again: false once `early_restart_limit` early restarts followed
	 * each other. */
	[[nodiscard]] bool may_restart(Clock::time_point now) noexcept
	{
		if (now - started_ < early_restart_window)
			++early_in_a_row_;
		else
			early_in_a_row_ = 0;
		return early_in_a_row_ < early_restart_limit;
	}

	/* Restarts in a row that came early. */
	[[nodiscard]] unsigned early_restarts() const noexcept
	{
		return early_in_a_row_;
	}

private:
	Clock::time_point started_{};
	unsigned early_in_a_row_{};
};

} // namespace uml::emulator
