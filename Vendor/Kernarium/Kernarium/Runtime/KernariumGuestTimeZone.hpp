// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <string_view>

namespace kernarium::timezone {

/* The kernel argument through which the runtime gives an Android guest the
 * host's time zone. The launcher owns it: the release's command-line file
 * cannot set it (it is a launcher prefix). It becomes
 * ro.boot.kernarium.timezone, which the guest's time zone script reads.
 * android/lineage-24/README.md owns what the guest does with it. */
inline constexpr std::string_view prefix {"androidboot.kernarium.timezone="};

/* The longest identifier the boot request may carry. The longest IANA name is
 * 32 characters. */
inline constexpr std::size_t maximumLength = 64;

/* Whether `identifier` is an IANA time zone name in the form the boot request
 * carries: 1 through 64 characters of A-Z a-z 0-9 _ + - and /, slash-separated
 * components that are never empty and start with a letter or digit. That is
 * every name of the IANA database (for example America/Argentina/Buenos_Aires,
 * Etc/GMT+5, EST5EDT, GB-Eire). Anything else, a space or a quotation mark
 * above all, could not be told apart from another kernel argument. Whether the
 * guest's own tzdata has the name is the guest's to decide. */
[[nodiscard]] inline bool validIdentifier(std::string_view identifier) noexcept
{
    if (identifier.empty() || identifier.size() > maximumLength)
        return false;
    bool componentStart = true;
    for (const char character : identifier) {
        if (character == '/') {
            if (componentStart)
                return false;
            componentStart = true;
            continue;
        }
        const bool alphanumeric = (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9');
        if (!alphanumeric && (componentStart || (character != '_' &&
                                                 character != '+' &&
                                                 character != '-')))
            return false;
        componentStart = false;
    }
    return !componentStart;
}

} // namespace kernarium::timezone
