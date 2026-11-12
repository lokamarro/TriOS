// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace uml::video {

/* Whether an application really decoded in hardware cannot be answered from
 * inside the guest: a player that silently falls back to software shows the
 * same correct picture, and the guest only sees that it opened a device node.
 * Setting `UML_VIDEO_ACTIVITY_LOG` to a writable path makes the device append
 * what it advertised, every stream a guest opened, and what each stream
 * actually decoded. It is a file rather than stderr because the emulator's
 * stderr shares the guest console and loses lines under load. Nothing is
 * opened, formatted, or written when the variable is unset.
 */
#ifdef UML_APP_STORE_RUNTIME
/* An App Store runtime has no activity log: it opens no file, and each line is
 * nothing. */
class ActivityLog final {
public:
    template<typename... Arguments> void advertised(const Arguments&...) noexcept {}
    template<typename... Arguments> void opened(const Arguments&...) noexcept {}
    template<typename... Arguments> void closed(const Arguments&...) noexcept {}
    template<typename... Arguments> void encoderAdvertised(const Arguments&...) noexcept {}
    template<typename... Arguments> void encoderOpened(const Arguments&...) noexcept {}
    template<typename... Arguments> void encoderStarted(const Arguments&...) noexcept {}
    template<typename... Arguments> void encoderClosed(const Arguments&...) noexcept {}
};
#else
class ActivityLog final {
public:
    ActivityLog()
    {
        const char* path = std::getenv("UML_VIDEO_ACTIVITY_LOG");
        if (path != nullptr && path[0] != '\0') file_ = std::fopen(path, "ae");
    }
    ~ActivityLog() { if (file_ != nullptr) std::fclose(file_); }
    ActivityLog(const ActivityLog&) = delete;
    ActivityLog& operator=(const ActivityLog&) = delete;

    void advertised(const char* codec, const char* profile, std::uint32_t width,
                    std::uint32_t height, std::uint32_t bitDepth)
    {
        if (file_ == nullptr) return;
        std::fprintf(file_, "uml-video %.3f advertise codec=%s profile=%s max=%ux%u depth=%u\n",
            elapsed(), codec, profile, width, height, bitDepth);
        std::fflush(file_);
    }
    void opened(std::uint32_t stream, const char* codec, const char* profile)
    {
        if (file_ == nullptr) return;
        std::fprintf(file_, "uml-video %.3f stream=%u open codec=%s profile=%s\n",
            elapsed(), stream, codec, profile);
        std::fflush(file_);
    }
    /* The capture fields are appended, so a reader that only knows the older
     * line keeps working. They answer, without any tool inside the guest,
     * where a client's capture memory lives and whether it ever gives a
     * filled buffer back: a client that queues each buffer once and stops has
     * starved its own pool, while re-queues that stop being filled are the
     * device's or the driver's fault. */
    void closed(std::uint32_t stream, const char* reason, std::uint64_t packets,
                std::uint64_t packetBytes, std::uint64_t frames,
                std::uint32_t width, std::uint32_t height, const char* capture,
                std::uint64_t queuedBuffers, std::uint64_t distinctBuffers,
                std::uint64_t requeuedBuffers, std::size_t pending, const char* raw,
                std::uint64_t converted, const char* codec)
    {
        if (file_ == nullptr) return;
        std::fprintf(file_,
            "uml-video %.3f stream=%u close=%s packets=%llu bytes=%llu frames=%llu output=%ux%u"
            " capture=%s queued=%llu distinct=%llu requeued=%llu pending_at_close=%zu"
            " raw=%s converted=%llu coded=%s\n",
            elapsed(), stream, reason, static_cast<unsigned long long>(packets),
            static_cast<unsigned long long>(packetBytes),
            static_cast<unsigned long long>(frames), width, height, capture,
            static_cast<unsigned long long>(queuedBuffers),
            static_cast<unsigned long long>(distinctBuffers),
            static_cast<unsigned long long>(requeuedBuffers), pending, raw,
            static_cast<unsigned long long>(converted), codec);
        std::fflush(file_);
    }

    /* The encoder's lines carry the word "encoder" after the time, so a
     * reader of a log that holds both devices tells them apart. They answer
     * the same question for the encode side: that a stream really reached
     * the hardware encoder, with what settings, and how much it produced. */
    void encoderAdvertised(const char* codec, const std::string& profiles, const std::string& inputs,
                           std::uint32_t width, std::uint32_t height, const std::string& rateControls,
                           std::uint32_t bFrames, std::uint32_t layers, std::uint32_t sessions)
    {
        if (file_ == nullptr) return;
        std::fprintf(file_,
            "uml-video %.3f encoder advertise codec=%s profiles=%s inputs=%s max=%ux%u rc=%s bframes=%u"
            " layers=%u sessions=%u\n",
            elapsed(), codec, profiles.c_str(), inputs.c_str(), width, height, rateControls.c_str(),
            bFrames, layers, sessions);
        std::fflush(file_);
    }
    void encoderOpened(std::uint32_t stream, const char* codec)
    {
        if (file_ == nullptr) return;
        std::fprintf(file_, "uml-video %.3f encoder stream=%u open codec=%s\n", elapsed(), stream, codec);
        std::fflush(file_);
    }
    /* Written when the stream's first frame configures the platform encoder,
     * from the settled state: what the guest asked for is what the host was
     * given. */
    void encoderStarted(std::uint32_t stream, const char* codec, const char* profile, const char* rateControl,
                        std::uint64_t bitrate, std::uint32_t width, std::uint32_t height,
                        std::uint32_t rateNumerator, std::uint32_t rateDenominator, const char* input,
                        std::uint32_t bFrames, std::uint32_t layers, const char* status)
    {
        if (file_ == nullptr) return;
        std::fprintf(file_,
            "uml-video %.3f encoder stream=%u start codec=%s profile=%s rc=%s bitrate=%llu size=%ux%u"
            " fps=%u/%u input=%s bframes=%u layers=%u status=%s\n",
            elapsed(), stream, codec, profile, rateControl, static_cast<unsigned long long>(bitrate), width,
            height, rateNumerator, rateDenominator, input, bFrames, layers, status);
        std::fflush(file_);
    }
    void encoderClosed(std::uint32_t stream, const char* reason, const char* codec, std::uint64_t framesIn,
                       std::uint64_t framesOut, std::uint64_t keyFrames, std::uint64_t bytes,
                       std::uint64_t errors, std::size_t pendingOutputs)
    {
        if (file_ == nullptr) return;
        std::fprintf(file_,
            "uml-video %.3f encoder stream=%u close=%s coded=%s frames_in=%llu frames_out=%llu keyframes=%llu"
            " bytes=%llu errors=%llu pending_outputs_at_close=%zu\n",
            elapsed(), stream, reason, codec, static_cast<unsigned long long>(framesIn),
            static_cast<unsigned long long>(framesOut), static_cast<unsigned long long>(keyFrames),
            static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(errors), pendingOutputs);
        std::fflush(file_);
    }

private:
    [[nodiscard]] double elapsed() const noexcept
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }
    std::FILE* file_ {};
    std::chrono::steady_clock::time_point start_ {std::chrono::steady_clock::now()};
};
#endif

} // namespace uml::video
