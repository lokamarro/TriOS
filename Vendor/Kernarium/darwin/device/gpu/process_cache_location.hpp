// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace uml::gpu {

/* Where the system keeps this process's caches. Metal names its shader cache
 * after the bundle identifier below the Darwin user cache directory, and the
 * Mesa disk shader cache takes the same place beside it
 * (mesa_shader_cache.hpp). */

/* The Darwin user cache directory (`confstr(_CS_DARWIN_USER_CACHE_DIR)`), or
 * nothing when the system does not name one. */
[[nodiscard]] std::optional<std::filesystem::path>
darwin_user_cache_directory();

/* The identifier CoreFoundation reads for a command-line process from the
 * Info.plist embedded in its executable, or nothing for a process without
 * one. */
[[nodiscard]] std::optional<std::string> main_bundle_identifier();

} // namespace uml::gpu
