// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Types shared by the encoder session and its discovery. Not part of the
// platform interface: nothing outside the Apple video encoder backend and its
// tests includes this.

#include "video_encoder.hpp"

#include <memory>
#include <string>
#include <vector>

namespace uml::platform::apple_encoder {

// What discovery learned about one codec: the interface-visible capability
// plus what only the backend needs.
struct CodecDiscovery {
    VideoEncodeCapability capability;
    // Profiles the low-latency encoder writes, which is a subset of the
    // profiles above: it silently turns High 4:2:2 into High, for example.
    // Empty when there is no low-latency encoder for the codec.
    std::vector<VideoEncodeProfile> lowLatencyProfiles;
};

struct Discovery {
    std::vector<CodecDiscovery> codecs;

    [[nodiscard]] const CodecDiscovery* find(VideoCodec codec) const noexcept
    {
        for (const auto& entry : codecs)
            if (entry.capability.codec == codec) return &entry;
        return nullptr;
    }
};

// The discovery of this process, from the cache or computed; waits for it.
[[nodiscard]] std::shared_ptr<const Discovery> discovery();

// Removes the cache file, so the next process rediscovers the host. Called when
// a session the discovery promised cannot be created.
void invalidateDiscoveryCache() noexcept;

// The session discovery drives. It is the production encoder with one
// difference: `configureUnchecked` skips the check against the discovery, which
// is what discovery is still computing. Every other call is the public one, so
// a probe exercises the same pipeline a guest's stream does.
class ProbeEncoder : public VideoEncoder {
public:
    [[nodiscard]] virtual VideoEncodeStatus configureUnchecked(const VideoEncodeSettings& settings) = 0;
    // Identifier of the VideoToolbox encoder behind the configured session,
    // for example "com.apple.videotoolbox.videoencoder.ave.avc"; empty when
    // none is configured.
    [[nodiscard]] virtual std::string encoderIdentifier() const = 0;
    // Why the last configure or session step failed, for the line a test or
    // a probe prints; empty when nothing failed.
    [[nodiscard]] virtual std::string diagnostic() const = 0;
#if UML_VIDEO_ENCODER_TESTING
    virtual void loseSessionForTest(unsigned count) = 0;
#endif
};

[[nodiscard]] std::unique_ptr<ProbeEncoder> makeProbeEncoder();

// The VideoToolbox ProfileLevel value naming a profile with the level left to
// the encoder, for example "H264_High_AutoLevel".
[[nodiscard]] std::string autoLevelName(VideoEncodeProfile profile);

// The discovery computation itself, run once on its own thread by the session
// layer's `discovery()` when no valid cache exists.
[[nodiscard]] Discovery runDiscovery();

} // namespace uml::platform::apple_encoder
