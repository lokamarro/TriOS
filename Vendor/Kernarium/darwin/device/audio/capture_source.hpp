// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace uml::audio {

struct CaptureParameters final {
    std::uint32_t periodBytes {};
    std::uint32_t sampleRate {};
    std::uint8_t channels {};
    std::uint8_t bytesPerSample {};
};

/* Where the guest's captured audio comes from.
 *
 * The device owns the capture clock: while the guest records, it completes
 * one period of the guest's receive queue per period of real time, whatever
 * the source holds, and fills the period from read(). A source that has
 * nothing, or less than a period, leaves the rest zero, and a source that
 * is slow or absent never stalls or speeds up the guest. Replacing the
 * source therefore changes what the guest hears and nothing else.
 *
 * Every call but read() comes from the transport worker, in the order the
 * guest's PCM requests arrive; read() comes from the same thread. A source
 * that gathers audio on a thread of its own hands it over through a bounded
 * single-producer ring, so neither side allocates or locks. The calls
 * cannot fail: a source that cannot deliver audio (no permission, no device,
 * switched off) returns nothing from read(), and the guest records silence. */
class CaptureSource {
public:
    virtual ~CaptureSource() = default;

    /* The guest chose the stream's parameters (48 kHz signed 16-bit, one
     * or two channels). */
    virtual void configure(const CaptureParameters& parameters) noexcept = 0;
    virtual void prepare() noexcept = 0;
    /* The guest started recording. */
    virtual void start() noexcept = 0;
    /* The guest stopped; audio captured from now on is not wanted. */
    virtual void stop() noexcept = 0;
    virtual void release() noexcept = 0;
    /* The transport reset the device. */
    virtual void reset() noexcept = 0;
    /* The source's own wake-up reached the transport worker, such as the
     * user's microphone setting changing under a recording guest. */
    virtual void handleEvents() noexcept {}
    /* Moves the oldest captured audio, interleaved in the configured
     * layout, into the front of `pcm` and returns how many bytes it wrote,
     * which is at most pcm.size(). */
    [[nodiscard]] virtual std::size_t read(
        std::span<std::uint8_t> pcm) noexcept = 0;
};

/* The default source: nothing is ever captured, so the guest records zeros. */
[[nodiscard]] std::unique_ptr<CaptureSource> makeSilentCaptureSource();

} // namespace uml::audio
