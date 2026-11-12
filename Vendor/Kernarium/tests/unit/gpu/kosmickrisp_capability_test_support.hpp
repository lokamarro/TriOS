// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Helpers for the tests that each check one capability in the JSON report of
// the KosmicKrisp capability probe.

#include "kosmickrisp_capability_probe.hpp"
#include "require.hpp"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace uml::gpu::testing {

using ::uml::testing::require;

[[nodiscard]] inline std::string probeReport()
{
    std::ostringstream output;
    require(run_kosmickrisp_capability_probe(output) == 0);
    return output.str();
}

[[nodiscard]] inline std::size_t countOccurrences(
    std::string_view text,
    std::string_view needle)
{
    std::size_t count = 0;
    for (std::size_t position = 0;
         (position = text.find(needle, position)) != std::string_view::npos;
         position += needle.size()) {
        ++count;
    }
    return count;
}

// The unsigned decimal that starts at position in text.
[[nodiscard]] inline std::uint64_t unsignedAt(
    std::string_view text,
    std::size_t position)
{
    auto last = position;
    while (last < text.size() && text[last] >= '0' && text[last] <= '9')
        ++last;
    require(last != position);
    std::uint64_t value = 0;
    for (auto index = position; index < last; ++index)
        value = value * 10U + static_cast<std::uint64_t>(text[index] - '0');
    return value;
}

// The unsigned decimal that follows the first occurrence of field in text.
[[nodiscard]] inline std::uint64_t unsignedField(
    std::string_view text,
    std::string_view field)
{
    const auto position = text.find(field);
    require(position != std::string_view::npos);
    return unsignedAt(text, position + field.size());
}

} // namespace uml::gpu::testing
