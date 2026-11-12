// SPDX-License-Identifier: GPL-2.0-or-later

#include "microphone_capture_source.hpp"

#include <utility>

namespace uml::audio {
namespace {

// Room for 340 ms of 48 kHz stereo. The system delivers the microphone in
// bursts of 100 ms or more.
constexpr std::size_t ringBytes = 64U * 1024U;
// The oldest audio beyond this much is dropped, so that a microphone running
// a little fast, or a guest that stalled, cannot add latency: a burst and a
// half, at the stream's rate.
constexpr std::uint32_t backlogMilliseconds = 250;

class MicrophoneCaptureSource final : public CaptureSource {
public:
    MicrophoneCaptureSource(
        std::shared_ptr<MicrophoneSwitch> microphone,
        std::unique_ptr<MicrophoneInput> input,
        std::shared_ptr<const Waker> waker)
        : microphone_(std::move(microphone))
        , input_(std::move(input))
        , waker_(std::move(waker))
        , ring_(ringBytes)
    {
        microphone_->setWaker(waker_);
    }

    ~MicrophoneCaptureSource() override
    {
        microphone_->clearWaker(waker_.get());
        stopInput();
    }

    void configure(const CaptureParameters& parameters) noexcept override
    {
        parameters_ = parameters;
    }

    void prepare() noexcept override {}

    void start() noexcept override
    {
        recording_ = true;
        reconcile();
    }

    void stop() noexcept override { finish(); }
    void release() noexcept override { finish(); }
    void reset() noexcept override { finish(); }
    void handleEvents() noexcept override { reconcile(); }

    [[nodiscard]] std::size_t read(std::span<std::uint8_t> pcm) noexcept override
    {
        if (!capturing_)
            return 0;
        const std::size_t limit = std::size_t {parameters_.sampleRate} *
            parameters_.channels * parameters_.bytesPerSample *
            backlogMilliseconds / 1000U;
        if (const auto held = ring_.size(); held > limit)
            ring_.discard(held - limit);
        return ring_.read(pcm);
    }

private:
    void finish() noexcept
    {
        recording_ = false;
        reconcile();
    }

    /* The microphone runs exactly while the guest records and the user's
     * setting allows it; a start that fails is tried again on the next
     * change. */
    void reconcile() noexcept
    {
        const bool wanted = recording_ && microphone_->active();
        if (wanted && !capturing_) {
            ring_.clear();
            capturing_ = input_->start(parameters_.channels, ring_);
        } else if (!wanted) {
            stopInput();
        }
    }

    void stopInput() noexcept
    {
        if (!capturing_)
            return;
        input_->stop();
        capturing_ = false;
        ring_.clear();
    }

    std::shared_ptr<MicrophoneSwitch> microphone_;
    std::unique_ptr<MicrophoneInput> input_;
    std::shared_ptr<const Waker> waker_;
    CaptureRing ring_;
    CaptureParameters parameters_ {};
    bool recording_ {};
    bool capturing_ {};
};

} // namespace

std::unique_ptr<CaptureSource> makeMicrophoneCaptureSource(
    std::shared_ptr<MicrophoneSwitch> microphone,
    std::unique_ptr<MicrophoneInput> input,
    std::shared_ptr<const Waker> waker)
{
    return std::make_unique<MicrophoneCaptureSource>(
        std::move(microphone), std::move(input), std::move(waker));
}

} // namespace uml::audio
