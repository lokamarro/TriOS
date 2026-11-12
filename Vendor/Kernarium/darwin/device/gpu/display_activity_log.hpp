// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace uml::gpu {

/* Only the host can say why a guest frame never became a visible pixel: the
 * guest sees a successful scanout flush whether the frame was presented,
 * dropped, refused by the display, or read back out of memory that holds no
 * pixels. Setting `UML_GPU_DISPLAY_ACTIVITY_LOG` to a writable path makes the
 * device append every scanout it is given, the path and outcome of every
 * flush, why a native texture was refused, and the display's own frame
 * counters. It is a file rather than stderr because the emulator's stderr
 * shares the guest console and loses lines under load. Nothing is opened,
 * formatted, or written when the variable is unset.
 *
 * A scanout flush repeats at the guest's frame rate, so an unchanged line is
 * counted instead of written, and the count is emitted when the line changes
 * or once a second, whichever comes first. */
#ifdef UML_APP_STORE_RUNTIME
/* An App Store runtime has no activity log: it opens no file, and a record is
 * nothing. */
class DisplayActivityLog final {
public:
    [[nodiscard]] static constexpr bool enabled() noexcept { return false; }

    template<typename... Arguments>
    static constexpr void record(const char*, const Arguments&...) noexcept
    {
    }
};
#else
class DisplayActivityLog final {
public:
    DisplayActivityLog() noexcept
    {
        const char* path = std::getenv("UML_GPU_DISPLAY_ACTIVITY_LOG");
        if (path != nullptr && path[0] != '\0')
            file_ = std::fopen(path, "ae");
    }

    ~DisplayActivityLog()
    {
        if (file_ == nullptr)
            return;
        std::lock_guard lock(mutex_);
        emitRepeatsLocked(elapsed());
        std::fclose(file_);
    }

    DisplayActivityLog(const DisplayActivityLog&) = delete;
    DisplayActivityLog& operator=(const DisplayActivityLog&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return file_ != nullptr; }

    __attribute__((format(printf, 2, 3)))
    void record(const char* format, ...) noexcept
    {
        if (file_ == nullptr)
            return;
        std::array<char, lineLimit> line {};
        std::va_list arguments;
        va_start(arguments, format);
        const int written =
            std::vsnprintf(line.data(), line.size(), format, arguments);
        va_end(arguments);
        if (written < 0)
            return;
        const double now = elapsed();
        std::lock_guard lock(mutex_);
        if (std::strcmp(line.data(), previous_.data()) == 0) {
            ++repeats_;
            if (now - reportedAt_ >= repeatInterval)
                emitRepeatsLocked(now);
            return;
        }
        emitRepeatsLocked(now);
        previous_ = line;
        reportedAt_ = now;
        std::fprintf(file_, "uml-display %.3f %s\n", now, line.data());
        std::fflush(file_);
    }

private:
    static constexpr std::size_t lineLimit = 512;
    static constexpr double repeatInterval = 1.0;

    void emitRepeatsLocked(double now) noexcept
    {
        if (repeats_ == 0)
            return;
        std::fprintf(
            file_,
            "uml-display %.3f repeated x%llu %s\n",
            now,
            static_cast<unsigned long long>(repeats_),
            previous_.data());
        std::fflush(file_);
        repeats_ = 0;
        reportedAt_ = now;
    }

    [[nodiscard]] double elapsed() const noexcept
    {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - start_)
            .count();
    }

    std::FILE* file_ {};
    std::mutex mutex_;
    std::chrono::steady_clock::time_point start_ {
        std::chrono::steady_clock::now()};
    std::array<char, lineLimit> previous_ {};
    std::uint64_t repeats_ {};
    double reportedAt_ {};
};
#endif

[[nodiscard]] inline DisplayActivityLog& displayActivityLog() noexcept
{
    static DisplayActivityLog log;
    return log;
}

/* The display's own frame counters, as the line that tells a display which
 * never received a scanout from one which discarded every frame. */
inline void recordDisplayStatistics(
    const char* display,
    bool final,
    const DisplayStatistics& total) noexcept
{
    displayActivityLog().record(
        "stats display=%s%s guest=%llu presented=%llu dropped=%llu "
        "cpu-fallback=%llu",
        display,
        final ? " final" : "",
        static_cast<unsigned long long>(total.guestFrames),
        static_cast<unsigned long long>(total.presentedFrames),
        static_cast<unsigned long long>(total.droppedFrames),
        static_cast<unsigned long long>(total.cpuFallbackFrames));
}

} // namespace uml::gpu
