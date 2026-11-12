// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace uml::app {

enum class DisplayInput { mouse, touchscreen };

struct LaunchConfiguration {
	const char *rootfs_path {};
	/* The mem= argument a launch takes when it gives none and states no
	 * process memory limit; with a limit (--memory-limit= or
	 * UML_MEMORY_LIMIT_MIB) it takes the default guest RAM for that limit. */
	std::string_view default_guest_memory;
	bool top_byte_ignore{};
	DisplayInput input_mode{DisplayInput::mouse};
	/* A file or directory the launcher created for this run only, removed
	 * once the launch ends. */
	std::filesystem::path run_scratch;
	/* The command line of the boot after the guest restarted itself, in the
	 * form of runEmbeddedKernel's `command_line`, or empty to boot the same
	 * command line again. It must ask for the host devices the first command
	 * line did: the display, the network and the forwards are created once,
	 * and only the kernel's arguments may differ. A throw ends the launch
	 * with status 1. */
	std::function<std::vector<std::string>()> restart_command_line;
};

/* Runs the embedded kernel with `command_line` (the program name first,
 * then the host device options and the kernel's arguments) and exits the
 * process with the guest's exit status, or returns a status of 1 when the
 * launch fails before the guest runs.
 *
 * A guest that restarts itself is booted again in the same process. The
 * emulator handle, the user network with its host forwards, and the display
 * are created once and kept; the Virtio services and the kernel's command
 * line are made for each boot, and the network link is reset between boots so
 * the old guest's connections end. A restart less than 15 s after its boot
 * started is early, and five in a row end the launch with status 3
 * (emulator/src/restart_loop_guard.hpp). A power-off ends the launch as it
 * always did, and a kernel panic still ends the process. */
int runEmbeddedKernel(const std::vector<std::string> &command_line,
		      char *envp[], const LaunchConfiguration &configuration);

/* Whether one of argv[1..argc) is exactly `argument`. */
[[nodiscard]] bool has_argument(
	int argc, char *const argv[], std::string_view argument);
/* Whether the run only prints information (--help, --version or
 * --showconfig) and boots no guest. */
[[nodiscard]] bool informational(int argc, char *const argv[]);

} // namespace uml::app
