// SPDX-License-Identifier: GPL-2.0-or-later

// The scanout mask of the Virgl capset tells the guest which formats the host
// can put on a display. The guest believes it: minigbm keeps the SCANOUT use
// on exactly the buffer combinations the mask names, and strips it from the
// others. A format with a scanout bit that the display does not present is
// therefore advertised falsely, and it costs the guest more than the scanout
// it never gets. minigbm emulates NV12 over R8 buffers and refuses a
// combination that has both emulation and scanout, so an NV12 entry that kept
// SCANOUT disappeared, and with it the video encoder, video decoder and
// camera uses that entry carried.
//
// The contract under test: the scanout mask names the 8-bit RGB layouts the
// display presents, and no YUV format.

#include "host_gl_test_support.hpp"
#include "protocol.hpp"
#include "virgl_renderer.hpp"

#include <virgl_hw.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <vector>

namespace {

using uml::gpu::testing::skipWithoutRenderer;

int failureCount = 0;

void check(bool condition, const char* description)
{
    if (condition)
        return;
    std::fprintf(stderr, "Scanout caps: %s\n", description);
    ++failureCount;
}

// The formats Virtio-GPU's display presents; see supportedFormat() in the
// device. The set is the contract, so it is spelled out here and not derived
// from the code under test.
constexpr std::array presentedFormats {
    VIRGL_FORMAT_B8G8R8A8_UNORM,
    VIRGL_FORMAT_B8G8R8X8_UNORM,
    VIRGL_FORMAT_A8R8G8B8_UNORM,
    VIRGL_FORMAT_X8R8G8B8_UNORM,
    VIRGL_FORMAT_R8G8B8A8_UNORM,
    VIRGL_FORMAT_X8B8G8R8_UNORM,
    VIRGL_FORMAT_A8B8G8R8_UNORM,
    VIRGL_FORMAT_R8G8B8X8_UNORM,
};

// The YUV formats a Linux or Android guest allocates video and camera
// buffers in: NV12 and P010 are the two minigbm names.
constexpr std::array yuvFormats {
    VIRGL_FORMAT_Y8_U8V8_420_UNORM,
    VIRGL_FORMAT_Y8_V8U8_420_UNORM,
    VIRGL_FORMAT_Y8_U8_V8_420_UNORM,
    VIRGL_FORMAT_Y8_V8_U8_420_UNORM,
    VIRGL_FORMAT_X6Y10_X6U10X6V10_420_UNORM,
    VIRGL_FORMAT_U8Y8V8Y8_422_UNORM,
    VIRGL_FORMAT_Y8U8Y8V8_422_UNORM,
};

[[nodiscard]] bool scanoutBit(const virgl_caps_v2& caps, std::uint32_t format)
{
    return ((caps.scanout.bitmask[format / 32U] >> (format % 32U)) & 1U) != 0U;
}

} // namespace

int main()
{
    std::vector<std::uint8_t> data;
    try {
        uml::gpu::VirglRenderer renderer;
        data = renderer.capset(uml::gpu::protocol::virgl2CapsetId).data;
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }

    virgl_caps_v2 caps {};
    check(data.size() >= offsetof(virgl_caps_v2, scanout) + sizeof(caps.scanout),
        "the capset is too short to hold a scanout mask");
    if (failureCount != 0)
        return 1;
    std::memcpy(&caps, data.data(), std::min(data.size(), sizeof(caps)));

    for (const auto format : presentedFormats)
        check(scanoutBit(caps, static_cast<std::uint32_t>(format)),
            "a format the display presents is not advertised for scanout");

    for (const auto format : yuvFormats)
        check(!scanoutBit(caps, static_cast<std::uint32_t>(format)),
            "a YUV format is advertised for scanout, which the display cannot "
            "present and which makes the guest drop its video uses");

    // No format outside the presented set may be advertised either.
    std::uint32_t extra = 0;
    for (std::uint32_t format = 0; format < VIRGL_FORMAT_MAX; ++format) {
        if (!scanoutBit(caps, format))
            continue;
        bool presented = false;
        for (const auto known : presentedFormats)
            presented = presented || static_cast<std::uint32_t>(known) == format;
        if (!presented && extra++ < 8)
            std::fprintf(stderr, "Scanout caps: format %u is advertised\n", format);
    }
    check(extra == 0,
        "formats the display does not present are advertised for scanout");

    if (failureCount != 0)
        return 1;
    std::fputs("Scanout caps: ok\n", stderr);
    return 0;
}
