// SPDX-License-Identifier: GPL-2.0-or-later

#include "playback_sink.hpp"

namespace uml::audio {
namespace {

class NullPlaybackSink final : public PlaybackSink {
public:
    [[nodiscard]] bool configure(const PlaybackParameters&) override
    {
        return true;
    }

    [[nodiscard]] bool prepare() override { return true; }
    [[nodiscard]] bool start() override { return true; }
    void stop() noexcept override {}
    void release() noexcept override {}
    void reset() noexcept override {}

    [[nodiscard]] PlaybackEnqueueResult enqueue(
        std::uint64_t,
        std::span<const std::uint8_t>) override
    {
        return PlaybackEnqueueResult::discarded;
    }

    void handleEvents() override {}

    [[nodiscard]] bool takeCompletion(PlaybackCompletion&) override
    {
        return false;
    }
};

} // namespace

std::unique_ptr<PlaybackSink> makeNullPlaybackSink()
{
    return std::make_unique<NullPlaybackSink>();
}

} // namespace uml::audio
