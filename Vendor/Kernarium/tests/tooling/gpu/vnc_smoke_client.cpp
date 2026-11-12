// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

extern "C" {
#include <rfb/rfbclient.h>
}

namespace {

std::uint64_t receivedUpdates;
std::string configuredPassword;

void framebufferUpdate(rfbClient*, int, int, int, int)
{
    ++receivedUpdates;
}

char* getPassword(rfbClient*)
{
    return ::strdup(configuredPassword.c_str());
}

void processOneMessage(rfbClient* client)
{
    const int ready = WaitForMessage(client, 100'000);
    if (ready < 0)
        throw std::runtime_error("VNC connection failed");
    if (ready > 0 && !HandleRFBServerMessage(client))
        throw std::runtime_error("VNC server message failed");
}

bool isRenderedFrame(const rfbClient& client)
{
    if (client.frameBuffer == nullptr)
        return false;

    // Sample the image on a 64x64 grid. A desktop or lock screen has
    // substantially more than these conservative diversity/nonblack bounds,
    // while an untouched zero-filled VNC scanout does not. Keep the area
    // bound low enough for Phosh's intentionally sparse black lock screen.
    constexpr std::size_t sampleGrid = 64;
    constexpr std::size_t minimumDistinctColors = 16;
    constexpr std::size_t minimumNonblackSamples = 32;
    std::set<std::uint32_t> colors;
    std::size_t nonblackSamples = 0;
    for (std::size_t sampleY = 0; sampleY < sampleGrid; ++sampleY) {
        const auto y = static_cast<std::size_t>(client.height) * sampleY /
            sampleGrid;
        for (std::size_t sampleX = 0; sampleX < sampleGrid; ++sampleX) {
            const auto x = static_cast<std::size_t>(client.width) * sampleX /
                sampleGrid;
            const auto offset =
                (y * static_cast<std::size_t>(client.width) + x) * 4U;
            const auto* pixel = client.frameBuffer + offset;
            const auto color =
                static_cast<std::uint32_t>(pixel[0]) |
                (static_cast<std::uint32_t>(pixel[1]) << 8U) |
                (static_cast<std::uint32_t>(pixel[2]) << 16U);
            colors.insert(color);
            if (color != 0)
                ++nonblackSamples;
        }
    }
    return colors.size() >= minimumDistinctColors &&
        nonblackSamples >= minimumNonblackSamples;
}

void writePpm(const rfbClient& client, const std::string& path)
{
    std::ofstream output(path, std::ios::binary);
    if (!output)
        throw std::runtime_error("cannot create VNC capture: " + path);
    output << "P6\n" << client.width << ' ' << client.height << "\n255\n";
    for (int y = 0; y < client.height; ++y) {
        for (int x = 0; x < client.width; ++x) {
            const auto offset =
                (static_cast<std::size_t>(y) * client.width + x) * 4U;
            const auto* pixel = client.frameBuffer + offset;
            // LibVNCClient stores the requested little-endian BGRX format.
            const char rgb[] {
                static_cast<char>(pixel[2]),
                static_cast<char>(pixel[1]),
                static_cast<char>(pixel[0]),
            };
            output.write(rgb, sizeof(rgb));
        }
    }
    if (!output)
        throw std::runtime_error("failed to write VNC capture: " + path);
}

} // namespace

int main(int argumentCount, char** arguments)
{
    int argumentIndex = 1;
    if (argumentCount >= 3 &&
        std::strcmp(arguments[argumentIndex], "--password") == 0) {
        configuredPassword = arguments[argumentIndex + 1];
        argumentIndex += 2;
    }
    const int remainingArguments = argumentCount - argumentIndex;
    const bool captureMode =
        remainingArguments == 3 &&
        std::strcmp(arguments[argumentIndex], "--capture") == 0;
    const bool inputMode =
        remainingArguments == 2 &&
        std::strcmp(arguments[argumentIndex], "--input") == 0;
    const bool clickMode =
        remainingArguments == 4 &&
        std::strcmp(arguments[argumentIndex], "--click") == 0;
    if (remainingArguments != 1 && !captureMode && !inputMode && !clickMode) {
        std::cerr << "usage: " << arguments[0]
                  << " [--password PASS] PORT\n       " << arguments[0]
                  << " [--password PASS] --capture OUTPUT.ppm PORT\n       "
                  << arguments[0]
                  << " [--password PASS] --input PORT\n       "
                  << arguments[0]
                  << " [--password PASS] --click X Y PORT\n";
        return 2;
    }
    try {
        const std::string capturePath =
            captureMode ? arguments[argumentIndex + 1] : "";
        const int port =
            std::stoi(arguments[
                argumentIndex +
                (captureMode ? 2 : inputMode ? 1 : clickMode ? 3 : 0)]);
        rfbClient* client = rfbGetClient(8, 3, 4);
        if (client == nullptr)
            throw std::runtime_error("rfbGetClient failed");
        if (!configuredPassword.empty())
            client->GetPassword = getPassword;
        client->serverHost = ::strdup("127.0.0.1");
        client->serverPort = port;
        client->GotFrameBufferUpdate = framebufferUpdate;
        client->format.bitsPerPixel = 32;
        client->format.depth = 24;
        client->format.bigEndian = FALSE;
        client->format.trueColour = TRUE;
        client->format.redMax = 255;
        client->format.greenMax = 255;
        client->format.blueMax = 255;
        client->format.redShift = 16;
        client->format.greenShift = 8;
        client->format.blueShift = 0;
        client->appData.encodingsString = const_cast<char*>("raw");
        if (!rfbInitClient(client, nullptr, nullptr))
            throw std::runtime_error("rfbInitClient failed");

        // The wait ends at the first update (a capture's first rendered
        // frame), or when the server drops the connection. The bound is only
        // for a server that stays connected and never sends one, so it is
        // generous for every mode: a full graphical session takes longer to
        // start than the deterministic 2D fixture, especially under an
        // instrumented UML guest.
        constexpr auto updateTimeout = std::chrono::seconds(90);
        const auto deadline = std::chrono::steady_clock::now() + updateTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            processOneMessage(client);
            if ((!captureMode && receivedUpdates != 0) ||
                (captureMode && receivedUpdates != 0 &&
                 isRenderedFrame(*client))) {
                break;
            }
        }
        if (receivedUpdates == 0)
            throw std::runtime_error("no framebuffer update received");
        if (client->width < 18 || client->height < 30 ||
            client->frameBuffer == nullptr) {
            throw std::runtime_error("invalid framebuffer dimensions");
        }

        if (inputMode || clickMode) {
            const int x = clickMode ?
                std::stoi(arguments[argumentIndex + 1]) : 24;
            const int y = clickMode ?
                std::stoi(arguments[argumentIndex + 2]) : client->height - 24;
            if (x < 0 || x >= client->width || y < 0 || y >= client->height)
                throw std::runtime_error("VNC click is outside the framebuffer");
            if (!SendPointerEvent(client, x, y, 0) ||
                !SendPointerEvent(client, x, y, 1) ||
                !SendPointerEvent(client, x, y, 0) ||
                (!clickMode &&
                 (!SendKeyEvent(client, 'a', TRUE) ||
                  !SendKeyEvent(client, 'a', FALSE)))) {
                throw std::runtime_error("failed to send VNC input events");
            }
            const auto inputDeadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < inputDeadline)
                processOneMessage(client);
            if (clickMode)
                std::cout << "VNC click sent at " << x << ',' << y << '\n';
            else
                std::cout << "VNC pointer and keyboard events sent\n";
            rfbClientCleanup(client);
            return 0;
        }

        if (captureMode) {
            if (!isRenderedFrame(*client))
                throw std::runtime_error(
                    "VNC framebuffer remained blank or insufficiently varied");
            writePpm(*client, capturePath);
            std::cout << "VNC rendered framebuffer captured at "
                      << client->width << 'x' << client->height
                      << " after " << receivedUpdates << " updates\n";
            rfbClientCleanup(client);
            return 0;
        }

        constexpr int x = 17;
        constexpr int y = 29;
        const auto offset =
            (static_cast<std::size_t>(y) * client->width + x) * 4U;
        const auto* pixel = client->frameBuffer + offset;
        if (pixel[0] != x || pixel[1] != y || pixel[2] != 0x5a) {
            throw std::runtime_error(
                "VNC pixel mismatch: " +
                std::to_string(pixel[0]) + "," +
                std::to_string(pixel[1]) + "," +
                std::to_string(pixel[2]));
        }
        std::cout << "VNC framebuffer verified at " << client->width << 'x'
                  << client->height << '\n';
        rfbClientCleanup(client);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "uml-vnc-smoke-client: " << error.what() << '\n';
        return 1;
    }
}
