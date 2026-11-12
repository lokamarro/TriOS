// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * How the runtime composes a statistics reading from the emulator's figures
 * and the display's, and when it will ask the guest for its memory: never
 * where a console request would wake a guest that sleeps, and never by
 * waiting for a lifecycle call. The sources and their locks are the
 * runtime's own; the rules are checked here against plain values.
 */
#include "KernariumStatistics.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <optional>
#include <string_view>

namespace {

using kernarium::statistics::DisplayFigures;
using kernarium::statistics::GraphicsFigures;
using kernarium::statistics::GraphicsHolderFigures;
using kernarium::statistics::graphicsReading;
using kernarium::statistics::GpuMemoryFigures;
using kernarium::statistics::displayActive;
using kernarium::statistics::guestMemoryResult;
using kernarium::statistics::memoryRefusal;
using kernarium::statistics::reading;
using kernarium::statistics::statisticsResult;
using uml::testing::require;

uml_emulator_statistics guestFigures()
{
    return {
        .struct_size = sizeof(uml_emulator_statistics),
        .guest_cpu_count = 4U,
        .guest_cpu_time_ns = 123456789012ULL,
        .guest_ram_bytes = 2147483648ULL,
        .guest_ram_held_bytes = 1610612736ULL,
    };
}

void aReadingCarriesEveryFigureOfItsSources()
{
    const auto statistics = reading(
        guestFigures(),
        DisplayFigures {.presentedFrames = 900U, .offeredFrames = 880U},
        true,
        2U,
        987654321ULL,
        7U,
        GpuMemoryFigures {
            .budgetBytes = 560005120ULL, .usedBytes = 511705088ULL});
    require(statistics.struct_size == sizeof(statistics));
    require(statistics.processor_count == 4U);
    require(statistics.boot_index == 2U);
    require(statistics.display_active == 1);
    require(statistics.timestamp_ns == 987654321ULL);
    require(statistics.processor_time_ns == 123456789012ULL);
    require(statistics.presented_frames == 900U);
    require(statistics.offered_frames == 880U);
    require(statistics.memory_bytes == 2147483648ULL);
    require(statistics.memory_held_bytes == 1610612736ULL);
    require(statistics.gpu_memory_refusals == 7U);
    require(statistics.gpu_memory_budget_bytes == 560005120ULL &&
            statistics.gpu_memory_used_bytes == 511705088ULL);
}

/* No GPU service reads as no frames and an unknown display, and the
 * processor figures stand: the guest can be running before its GPU is. */
void noDisplayReadsAsNoFramesAndAnUnknownScreen()
{
    const auto statistics =
        reading(guestFigures(), std::nullopt, std::nullopt, 0U, 1U);
    require(statistics.display_active == -1);
    require(statistics.presented_frames == 0U &&
        statistics.offered_frames == 0U);
    require(statistics.processor_time_ns == 123456789012ULL &&
        statistics.processor_count == 4U);
    require(statistics.gpu_memory_refusals == 0U,
        "no GPU service refused anything");
    require(statistics.gpu_memory_budget_bytes == 0U &&
            statistics.gpu_memory_used_bytes == 0U,
        "no GPU service gave the guest a budget");
}

/* A graphics memory reading carries the service's figures and its holders,
 * largest first, with names cut to fit the table and its room. */
void aGraphicsReadingCarriesTheServicesFigures()
{
    const std::array<GraphicsHolderFigures, 2> holders {{
        {.pid = 2204U, .contexts = 3U, .bytes = 136314880ULL,
            .idleMs = 61500ULL, .name = "org.chromium.chrome.long.name"},
        {.pid = 0U, .contexts = 1U, .bytes = 8388608ULL, .idleMs = 0ULL,
            .name = "RenderThread"},
    }};
    const auto memory = graphicsReading({
        .budgetBytes = 560005120ULL,
        .usageBytes = 524288000ULL,
        .contextBytes = 440401920ULL,
        .memoryRefusals = 2ULL,
        .guestReclaims = true,
        .holders = holders,
    });
    require(memory.struct_size == sizeof(memory));
    require(memory.budget_bytes == 560005120ULL &&
            memory.usage_bytes == 524288000ULL &&
            memory.context_bytes == 440401920ULL &&
            memory.memory_refusals == 2ULL);
    require(memory.guest_reclaims == 1U,
        "the guest's own reclaimer was not reported");
    require(memory.holder_count == 2U);
    require(memory.holders[0].pid == 2204U &&
            memory.holders[0].context_count == 3U &&
            memory.holders[0].bytes == 136314880ULL &&
            memory.holders[0].idle_ms == 61500ULL);
    require(std::string_view {memory.holders[0].name} == "org.chromium.ch",
        "a long name was not cut to the room the holder has");
    require(std::string_view {memory.holders[1].name} == "RenderThread" &&
            memory.holders[1].pid == 0U);
    require(memory.holders[2].pid == 0U && memory.holders[2].bytes == 0U,
        "an unused entry was not empty");
}

void noHoldersAndTooManyHoldersFitTheTable()
{
    const auto none = graphicsReading({.budgetBytes = 1ULL});
    require(none.holder_count == 0U && none.budget_bytes == 1ULL &&
            none.guest_reclaims == 0U);

    std::array<GraphicsHolderFigures, 12> many {};
    for (std::uint32_t index = 0; index < many.size(); ++index)
        many[index] = {.pid = index + 1U, .bytes = 100ULL - index};
    const auto full = graphicsReading({.holders = many});
    require(full.holder_count == KERNARIUM_RUNTIME_GRAPHICS_HOLDERS &&
            full.holders[7].pid == 8U,
        "the entries past the table's room were not dropped");
}

void theScreenReadsAsOneZeroOrUnknown()
{
    require(displayActive(true) == 1);
    require(displayActive(false) == 0);
    require(displayActive(std::nullopt) == -1);
    require(reading(guestFigures(), std::nullopt, false, 0U, 1U)
            .display_active == 0);
}

void theEmulatorsAnswersBecomeTheRuntimes()
{
    require(statisticsResult(UML_EMULATOR_OK) == 0);
    require(statisticsResult(-UML_EMULATOR_UNSUPPORTED) == -ENODEV,
        "a handle that does not run is no guest");
    require(statisticsResult(-UML_EMULATOR_INVALID_ARGUMENT) == -EINVAL);
    require(statisticsResult(-UML_EMULATOR_RUNTIME_ERROR) == -EIO);
    require(statisticsResult(-UML_EMULATOR_OUT_OF_MEMORY) == -EIO);

    require(guestMemoryResult(UML_EMULATOR_OK) == 0);
    require(guestMemoryResult(-UML_EMULATOR_UNSUPPORTED) == -ENOTSUP,
        "a kernel without the request is unsupported");
    require(guestMemoryResult(-UML_EMULATOR_RUNTIME_ERROR) == -ETIMEDOUT,
        "no reply in time is a timeout");
    require(guestMemoryResult(-UML_EMULATOR_INVALID_ARGUMENT) == -EINVAL);
    require(guestMemoryResult(-UML_EMULATOR_OUT_OF_MEMORY) == -EIO);
}

/* A request ends a suspend, so it is sent only to a guest that runs and is
 * not about to sleep. */
void aRunningGuestWithAScreenIsAsked()
{
    require(memoryRefusal(true, true, false, true) == 0);
    require(memoryRefusal(true, true, false, std::nullopt) == 0,
        "a display not yet known to be off forbids nothing");
}

void aGuestThatMaySleepIsNotAsked()
{
    require(memoryRefusal(true, true, true, true) == -EAGAIN,
        "a quiesced guest was asked");
    require(memoryRefusal(true, true, true, std::nullopt) == -EAGAIN);
    require(memoryRefusal(true, true, false, false) == -EAGAIN,
        "a guest whose screen is off was asked");
}

/* A lifecycle call holds the lock for seconds and is not waited for, and with
 * the lock not taken nothing else is known about the guest. */
void aBusyLifecycleIsNotWaitedFor()
{
    require(memoryRefusal(false, true, false, true) == -EAGAIN);
    require(memoryRefusal(false, false, false, true) == -EAGAIN,
        "an unread registration was taken for no guest");
}

void noGuestIsNoGuest()
{
    require(memoryRefusal(true, false, false, true) == -ENODEV);
    require(memoryRefusal(true, false, true, false) == -ENODEV,
        "a guest that does not run is neither quiesced nor dark");
}

}  // namespace

int main()
{
    constexpr uml::testing::NamedTest tests[] = {
        {"a-reading-carries-every-figure-of-its-sources",
            aReadingCarriesEveryFigureOfItsSources},
        {"no-display-reads-as-no-frames-and-an-unknown-screen",
            noDisplayReadsAsNoFramesAndAnUnknownScreen},
        {"a-graphics-reading-carries-the-services-figures",
            aGraphicsReadingCarriesTheServicesFigures},
        {"no-holders-and-too-many-holders-fit-the-table",
            noHoldersAndTooManyHoldersFitTheTable},
        {"the-screen-reads-as-one-zero-or-unknown",
            theScreenReadsAsOneZeroOrUnknown},
        {"the-emulators-answers-become-the-runtimes",
            theEmulatorsAnswersBecomeTheRuntimes},
        {"a-running-guest-with-a-screen-is-asked",
            aRunningGuestWithAScreenIsAsked},
        {"a-guest-that-may-sleep-is-not-asked", aGuestThatMaySleepIsNotAsked},
        {"a-busy-lifecycle-is-not-waited-for", aBusyLifecycleIsNotWaitedFor},
        {"no-guest-is-no-guest", noGuestIsNoGuest},
    };
    return uml::testing::runNamedTests(tests);
}
