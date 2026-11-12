// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "require.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

/* A minimal RFB 3.8 client for the tests that must see what a VNC viewer
 * sees, over a real connection to the display's listener. */
namespace uml::gpu::testing {

inline constexpr int vncClientTimeoutSeconds = 10;
inline constexpr auto vncClientTimeout =
    std::chrono::seconds(vncClientTimeoutSeconds);

class VncConnection final {
public:
    explicit VncConnection(std::uint16_t port)
        : descriptor_(::socket(AF_INET, SOCK_STREAM, 0))
    {
        uml::testing::require(descriptor_ >= 0, "VNC test socket failed");
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        uml::testing::require(
            ::connect(
                descriptor_,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0,
            "VNC test connect failed");
        const timeval timeout {.tv_sec = vncClientTimeoutSeconds, .tv_usec = 0};
        uml::testing::require(
            ::setsockopt(
                descriptor_, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                sizeof(timeout)) == 0,
            "VNC test receive timeout failed");
    }

    ~VncConnection()
    {
        if (descriptor_ >= 0)
            ::close(descriptor_);
    }

    VncConnection(const VncConnection&) = delete;
    VncConnection& operator=(const VncConnection&) = delete;

    void send(const void* bytes, std::size_t size)
    {
        const auto* cursor = static_cast<const std::uint8_t*>(bytes);
        while (size != 0) {
            const auto written = ::send(descriptor_, cursor, size, 0);
            uml::testing::require(written > 0, "VNC test send failed");
            cursor += written;
            size -= static_cast<std::size_t>(written);
        }
    }

    std::vector<std::uint8_t> receive(std::size_t size)
    {
        std::vector<std::uint8_t> bytes(size);
        std::size_t filled = 0;
        while (filled < size) {
            const auto read = ::recv(
                descriptor_, bytes.data() + filled, size - filled, 0);
            uml::testing::require(read > 0, "VNC test receive failed");
            filled += static_cast<std::size_t>(read);
        }
        return bytes;
    }

    /* Enough of RFB 3.8 to reach pixels: no security, shared session, and the
     * little-endian 32-bit BGRX format the raw encoding then delivers. */
    void handshake()
    {
        const auto version = receive(12);
        uml::testing::require(
            std::memcmp(version.data(), "RFB ", 4) == 0,
            "VNC test server is not an RFB server");
        send("RFB 003.008\n", 12);
        const auto offered = receive(1);
        uml::testing::require(
            offered[0] != 0, "VNC test server refused the handshake");
        const auto types = receive(offered[0]);
        uml::testing::require(
            std::find(types.begin(), types.end(), std::uint8_t {1}) !=
                types.end(),
            "VNC test server requires authentication");
        const std::uint8_t none = 1;
        send(&none, 1);
        const auto result = receive(4);
        uml::testing::require(
            result[0] == 0 && result[1] == 0 && result[2] == 0 &&
                result[3] == 0,
            "VNC test security handshake failed");
        const std::uint8_t shared = 1;
        send(&shared, 1);
        const auto initial = receive(24);
        width_ = static_cast<std::uint32_t>(initial[0]) << 8U | initial[1];
        height_ = static_cast<std::uint32_t>(initial[2]) << 8U | initial[3];
        const auto nameLength =
            static_cast<std::size_t>(initial[20]) << 24U |
            static_cast<std::size_t>(initial[21]) << 16U |
            static_cast<std::size_t>(initial[22]) << 8U |
            static_cast<std::size_t>(initial[23]);
        (void)receive(nameLength);
        const std::array<std::uint8_t, 20> pixelFormat {
            0, 0, 0, 0, 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0,
            0, 0, 0};
        send(pixelFormat.data(), pixelFormat.size());
        const std::array<std::uint8_t, 8> encodings {
            2, 0, 0, 1, 0, 0, 0, 0};
        send(encodings.data(), encodings.size());
    }

    void pointer(std::uint8_t buttons, std::uint16_t x, std::uint16_t y)
    {
        const std::array<std::uint8_t, 6> event {
            5,
            buttons,
            static_cast<std::uint8_t>(x >> 8U),
            static_cast<std::uint8_t>(x),
            static_cast<std::uint8_t>(y >> 8U),
            static_cast<std::uint8_t>(y),
        };
        send(event.data(), event.size());
    }

    /* One non-incremental full-screen request, then the rectangles it
     * answers with, composited into a BGRX image of the whole framebuffer. */
    std::vector<std::uint8_t> readFramebuffer()
    {
        const std::array<std::uint8_t, 10> request {
            3,
            0,
            0,
            0,
            0,
            0,
            static_cast<std::uint8_t>(width_ >> 8U),
            static_cast<std::uint8_t>(width_),
            static_cast<std::uint8_t>(height_ >> 8U),
            static_cast<std::uint8_t>(height_),
        };
        send(request.data(), request.size());
        std::vector<std::uint8_t> image(
            static_cast<std::size_t>(width_) * height_ * 4U);
        const auto deadline =
            std::chrono::steady_clock::now() + vncClientTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto kind = receive(1);
            uml::testing::require(
                kind[0] == 0, "unexpected RFB server message");
            const auto header = receive(3);
            const auto rectangles =
                static_cast<std::uint32_t>(header[1]) << 8U | header[2];
            for (std::uint32_t index = 0; index < rectangles; ++index) {
                const auto descriptor = receive(12);
                const auto x = static_cast<std::uint32_t>(descriptor[0]) << 8U |
                    descriptor[1];
                const auto y = static_cast<std::uint32_t>(descriptor[2]) << 8U |
                    descriptor[3];
                const auto rectangleWidth =
                    static_cast<std::uint32_t>(descriptor[4]) << 8U |
                    descriptor[5];
                const auto rectangleHeight =
                    static_cast<std::uint32_t>(descriptor[6]) << 8U |
                    descriptor[7];
                uml::testing::require(
                    descriptor[8] == 0 && descriptor[9] == 0 &&
                        descriptor[10] == 0 && descriptor[11] == 0,
                    "VNC test server did not use the raw encoding");
                const auto pixels = receive(
                    static_cast<std::size_t>(rectangleWidth) *
                    rectangleHeight * 4U);
                for (std::uint32_t row = 0; row < rectangleHeight; ++row) {
                    std::memcpy(
                        image.data() +
                            ((static_cast<std::size_t>(y) + row) * width_ + x) *
                                4U,
                        pixels.data() +
                            static_cast<std::size_t>(row) * rectangleWidth * 4U,
                        static_cast<std::size_t>(rectangleWidth) * 4U);
                }
            }
            if (rectangles != 0)
                return image;
        }
        throw std::runtime_error("no VNC framebuffer update arrived");
    }

private:
    int descriptor_ {-1};
    std::uint32_t width_ {};
    std::uint32_t height_ {};
};

} // namespace uml::gpu::testing
