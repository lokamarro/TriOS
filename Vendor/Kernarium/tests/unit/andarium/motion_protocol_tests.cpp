// SPDX-License-Identifier: GPL-2.0-or-later
#include "motion_protocol.hpp"
#include "require.hpp"

using uml::testing::require;

int main()
{
    // Independent wire fixture: +1, -2, +0.5 radians/second.
    std::array<uint8_t, 20> bytes{
        'A', 'N', 'S', '1', 4, 0, 0, 0,
        0, 0, 0x80, 0x3f, 0, 0, 0, 0xc0, 0, 0, 0, 0x3f};
    const auto sample = andarium::motion::decode(bytes);
    require(sample.has_value());
    require(sample->type == 4);
    require(sample->values == std::array<float, 3>{1, -2, 0.5f});
    require(!andarium::motion::decode(std::span(bytes).first(19)));
    bytes[0] = 0;
    require(!andarium::motion::decode(bytes));
    bytes[0] = 'A';
    bytes[4] = 99;
    require(!andarium::motion::decode(bytes));
    bytes[4] = 1;
    require(andarium::motion::decode(bytes).has_value());
    bytes[10] = 0x80;
    bytes[11] = 0x7f; // Infinity is never a physical measurement.
    require(!andarium::motion::decode(bytes));
    bytes[10] = 0xc0; // Neither is NaN.
    require(!andarium::motion::decode(bytes));

    // Independent control frame fixtures: the accelerometer alone at 20 ms
    // (the gyroscope's period is carried and not read), then both, at 66 667
    // us and 1 s.
    using andarium::motion::encode_control;
    require(encode_control({true, false}, {20'000'000, 10'000'000}) ==
            std::array<uint8_t, 16>{'A', 'N', 'C', '1', 1, 0, 0, 0,
                                    0x20, 0x4e, 0, 0, 0x10, 0x27, 0, 0});
    require(encode_control({true, true}, {66'667'000, 1'000'000'000}) ==
            std::array<uint8_t, 16>{'A', 'N', 'C', '1', 3, 0, 0, 0,
                                    0x6b, 0x04, 0x01, 0, 0x40, 0x42, 0x0f, 0});
    // Nothing in use, and periods the sensor framework would never ask for,
    // which the frame brings into 10 ms to 1 s.
    require(encode_control({false, false}, {0, 5'000'000'000}) ==
            std::array<uint8_t, 16>{'A', 'N', 'C', '1', 0, 0, 0, 0,
                                    0x10, 0x27, 0, 0, 0x40, 0x42, 0x0f, 0});
    require(encode_control({false, true}, {10'000'000, 10'000'000})[4] == 2);
}
