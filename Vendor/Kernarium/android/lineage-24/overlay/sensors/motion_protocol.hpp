// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace andarium::motion {
constexpr uint32_t port = 5556;
constexpr size_t record_size = 20;
struct Sample {
    uint32_t type;
    std::array<float, 3> values;
};
inline std::optional<Sample> decode(std::span<const uint8_t> bytes) {
    if (bytes.size() != record_size || bytes[0] != 'A' || bytes[1] != 'N' ||
        bytes[2] != 'S' || bytes[3] != '1') return std::nullopt;
    const auto word = [&bytes](size_t offset) {
        return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
               (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
    };
    Sample sample{word(4), {}};
    if (sample.type != 1 && sample.type != 4) return std::nullopt;
    for (size_t i = 0; i < sample.values.size(); ++i) {
        sample.values[i] = std::bit_cast<float>(word(8 + i * 4));
        if (!std::isfinite(sample.values[i])) return std::nullopt;
    }
    return sample;
}

// Guest-to-host control frame, sent on the same connection when the host
// connects and at every change of what applications use. 16 bytes, little
// endian: "ANC1", a uint32 mask of the active sensors (bit 0 accelerometer,
// bit 1 gyroscope), then the uint32 sampling period in microseconds of the
// accelerometer and of the gyroscope. The host reads a sensor only while it is
// active; a host that has not seen a frame reads both continuously.
constexpr size_t control_size = 16;
constexpr int64_t minimum_period_ns = 10'000'000;
constexpr int64_t maximum_period_ns = 1'000'000'000;
inline std::array<uint8_t, control_size> encode_control(
        const std::array<bool, 2>& active, const std::array<int64_t, 2>& period_ns) {
    std::array<uint8_t, control_size> frame{'A', 'N', 'C', '1'};
    const auto put = [&frame](size_t offset, uint32_t value) {
        for (size_t i = 0; i < 4; ++i) frame[offset + i] = uint8_t(value >> (8 * i));
    };
    put(4, (active[0] ? 1u : 0u) | (active[1] ? 2u : 0u));
    for (size_t i = 0; i < period_ns.size(); ++i) {
        const auto period = std::clamp(period_ns[i], minimum_period_ns, maximum_period_ns);
        put(8 + i * 4, uint32_t(period / 1000));
    }
    return frame;
}
} // namespace andarium::motion
