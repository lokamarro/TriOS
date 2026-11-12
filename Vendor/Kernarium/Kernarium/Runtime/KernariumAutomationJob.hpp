// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdlib>
#include <cstring>

namespace kernarium::automation {

/* The environment variable that asks an automation launch to start the
 * background job as soon as its guest runs, the action the job button takes.
 * Only the automation runtimes include this header, so only they contain the
 * name; the packaging script refuses a release runtime that does. */
inline constexpr char jobAtStartVariable[] = "KERNARIUM_BACKGROUND_JOB_AT_START";

/* The environment variable that asks an automation launch to hold the guest's
 * GPU that many milliseconds after the application enters the background
 * rather than at once, to find how long the host GPU stays usable there. The
 * same rule keeps its name out of release runtimes. */
inline constexpr char holdDelayVariable[] = "KERNARIUM_BACKGROUND_HOLD_DELAY_MS";
/* More than the time iOS lets a background task run would measure nothing. */
inline constexpr int holdDelayMaximumMs = 30000;

enum class JobAtStart { off, on, invalid };

/* "1" asks for the job, "0" and an unset variable do not, and anything else is
 * a mistake of the driver that the runtime reports and does not act on. */
[[nodiscard]] inline JobAtStart parseJobAtStart(const char* value) noexcept
{
    if (value == nullptr || std::strcmp(value, "0") == 0)
        return JobAtStart::off;
    return std::strcmp(value, "1") == 0 ? JobAtStart::on : JobAtStart::invalid;
}

struct HoldDelay {
    bool valid {true};
    int milliseconds {};
};

/* An unset variable and "0" ask for no delay. Otherwise the value is a whole
 * number of milliseconds in digits alone, up to holdDelayMaximumMs; anything
 * else is a mistake of the driver that the runtime reports and does not act
 * on, so a misspelt value cannot move the hold. */
[[nodiscard]] inline HoldDelay parseHoldDelayMs(const char* value) noexcept
{
    if (value == nullptr)
        return {};
    const std::size_t length = std::strlen(value);
    if (length == 0 || length > 5)
        return {.valid = false, .milliseconds = 0};
    int milliseconds = 0;
    for (std::size_t index = 0; index < length; ++index) {
        if (value[index] < '0' || value[index] > '9')
            return {.valid = false, .milliseconds = 0};
        milliseconds = milliseconds * 10 + (value[index] - '0');
    }
    if (milliseconds > holdDelayMaximumMs)
        return {.valid = false, .milliseconds = 0};
    return {.valid = true, .milliseconds = milliseconds};
}

} // namespace kernarium::automation
