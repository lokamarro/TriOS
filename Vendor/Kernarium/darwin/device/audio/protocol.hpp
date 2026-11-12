// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::audio::protocol {

// Device ID 25 is VIRTIO_ID_SOUND in the stable Virtio device ID registry.
constexpr std::uint32_t deviceId = 25;

constexpr std::uint32_t controlQueue = 0;
constexpr std::uint32_t eventQueue = 1;
constexpr std::uint32_t transmitQueue = 2;
constexpr std::uint32_t receiveQueue = 3;
constexpr std::size_t queueCount = 4;

// The playback stream and the capture stream share function group 0, which
// the guest driver exposes as the two directions of PCM device 0.
constexpr std::uint32_t streamId = 0;
constexpr std::uint32_t channelMapId = 0;
constexpr std::uint32_t inputStreamId = 1;
constexpr std::uint32_t inputChannelMapId = 1;
constexpr std::uint32_t streamCount = 2;
constexpr std::uint32_t sampleRate = 48'000;
constexpr std::uint8_t channelCount = 2;
constexpr std::uint8_t bytesPerSample = 2;
constexpr std::uint32_t frameBytes =
    channelCount * bytesPerSample;
constexpr std::uint32_t maximumBufferBytes = 1024U * 1024U;
constexpr std::uint32_t maximumPeriods = 256;

enum class Request : std::uint32_t {
    pcmInfo = 0x0100,
    pcmSetParameters,
    pcmPrepare,
    pcmRelease,
    pcmStart,
    pcmStop,
    channelMapInfo = 0x0200,
};

enum class Status : std::uint32_t {
    ok = 0x8000,
    badMessage,
    notSupported,
    ioError,
};

enum class Direction : std::uint8_t {
    output = 0,
    input = 1,
};

enum class PcmFormat : std::uint8_t {
    signed16 = 5,
};

enum class PcmRate : std::uint8_t {
    rate48000 = 7,
};

enum class ChannelPosition : std::uint8_t {
    frontLeft = 3,
    frontRight = 4,
};

struct [[gnu::packed]] Configuration final {
    std::uint32_t jacks;
    std::uint32_t streams;
    std::uint32_t channelMaps;
    std::uint32_t controls;
};

struct [[gnu::packed]] Header final {
    std::uint32_t code;
};

struct [[gnu::packed]] QueryInfo final {
    Header header;
    std::uint32_t startId;
    std::uint32_t count;
    std::uint32_t size;
};

struct [[gnu::packed]] Info final {
    std::uint32_t functionGroupNodeId;
};

struct [[gnu::packed]] PcmHeader final {
    Header header;
    std::uint32_t streamId;
};

struct [[gnu::packed]] PcmInfo final {
    Info header;
    std::uint32_t features;
    std::uint64_t formats;
    std::uint64_t rates;
    Direction direction;
    std::uint8_t minimumChannels;
    std::uint8_t maximumChannels;
    std::array<std::uint8_t, 5> padding;
};

struct [[gnu::packed]] PcmSetParameters final {
    PcmHeader header;
    std::uint32_t bufferBytes;
    std::uint32_t periodBytes;
    std::uint32_t features;
    std::uint8_t channels;
    PcmFormat format;
    PcmRate rate;
    std::uint8_t padding;
};

struct [[gnu::packed]] PcmTransfer final {
    std::uint32_t streamId;
};

struct [[gnu::packed]] PcmStatus final {
    Status status;
    std::uint32_t latencyBytes;
};

struct [[gnu::packed]] ChannelMapInfo final {
    Info header;
    Direction direction;
    std::uint8_t channels;
    std::array<std::uint8_t, 18> positions;
};

static_assert(sizeof(Configuration) == 16);
static_assert(sizeof(Header) == 4);
static_assert(sizeof(QueryInfo) == 16);
static_assert(sizeof(PcmHeader) == 8);
static_assert(sizeof(PcmInfo) == 32);
static_assert(sizeof(PcmSetParameters) == 24);
static_assert(sizeof(PcmTransfer) == 4);
static_assert(sizeof(PcmStatus) == 8);
static_assert(sizeof(ChannelMapInfo) == 24);

} // namespace uml::audio::protocol
