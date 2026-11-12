// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_encoder.hpp"

#include <vector>

namespace uml::testing {

/* An encoder host that offers H.264 (three profiles, every rate-control mode,
 * a B-frame run of three, two temporal layers), HEVC (Main and Main10, HDR
 * metadata, no quantizer-only mode, a lower bitrate ceiling, a B-frame run of
 * two) and JPEG. The two video codecs
 * differ in several limits on purpose: a control they share has to stop at the
 * more restrictive one. */
inline std::vector<uml::platform::VideoEncodeCapability> encoderHostCapabilities()
{
    using namespace uml::platform;
    VideoEncodeCapability h264;
    h264.codec = VideoCodec::h264;
    const std::vector<RawVideoLayout> eightBit {RawVideoLayout::nv12, RawVideoLayout::i420, RawVideoLayout::bgra,
                                                RawVideoLayout::rgba};
    h264.profiles = {
        {VideoEncodeProfile::h264ConstrainedBaseline, eightBit, 52, true},
        {VideoEncodeProfile::h264Main, eightBit, 52, true},
        {VideoEncodeProfile::h264High, eightBit, 52, true},
    };
    h264.minimumWidth = 64;
    h264.minimumHeight = 64;
    h264.maximumWidth = 4096;
    h264.maximumHeight = 2304;
    h264.sizeAlignment = 2;
    h264.rateControls = {VideoRateControl::variableBitrate, VideoRateControl::constantBitrate,
                         VideoRateControl::constantQuality, VideoRateControl::constantQp};
    h264.maximumBitrate = 100'000'000;
    h264.minimumQp = 0;
    h264.maximumQp = 51;
    h264.maximumBFrames = 3;
    h264.maximumTemporalLayers = 2;
    h264.entropyModeSelectable = true;
    h264.concurrentSessions = 4;

    VideoEncodeCapability hevc;
    hevc.codec = VideoCodec::hevc;
    hevc.profiles = {
        {VideoEncodeProfile::hevcMain, eightBit, 186, true},
        {VideoEncodeProfile::hevcMain10, {RawVideoLayout::p010}, 186, true},
    };
    hevc.minimumWidth = 64;
    hevc.minimumHeight = 64;
    hevc.maximumWidth = 8192;
    hevc.maximumHeight = 4320;
    hevc.sizeAlignment = 2;
    hevc.rateControls = {VideoRateControl::variableBitrate, VideoRateControl::constantBitrate,
                         VideoRateControl::constantQuality};
    hevc.maximumBitrate = 80'000'000;
    hevc.minimumQp = 0;
    hevc.maximumQp = 51;
    hevc.maximumBFrames = 2;
    hevc.maximumTemporalLayers = 1;
    hevc.hdrStaticMetadata = true;
    hevc.concurrentSessions = 4;

    VideoEncodeCapability jpeg;
    jpeg.codec = VideoCodec::jpeg;
    jpeg.profiles = {{VideoEncodeProfile::jpegBaseline, {RawVideoLayout::nv12, RawVideoLayout::bgra, RawVideoLayout::rgba}, 0, false}};
    jpeg.minimumWidth = 16;
    jpeg.minimumHeight = 16;
    jpeg.maximumWidth = 16384;
    jpeg.maximumHeight = 16384;
    jpeg.sizeAlignment = 2;
    jpeg.rateControls = {VideoRateControl::constantQuality};
    jpeg.concurrentSessions = 4;
    return {h264, hevc, jpeg};
}

} // namespace uml::testing
