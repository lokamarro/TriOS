// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* A scripted guest's side of one Virtio-GPU connection, as far as showing a
 * frame goes. It speaks the vhost-user protocol the service serves, gives the
 * device one page of memory and sends the control commands of a software
 * scanout, which the service's display then shows. */

#include "gpu_test_support.hpp"
#include "in_process_service.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unistd.h>

namespace uml::gpu::testing {

using ::uml::testing::require;
namespace protocol = ::uml::gpu::protocol;

/* The frame's size on each side, in pixels. */
inline constexpr std::uint32_t frameExtent = 8;
/* BGRA, as the Virtio-GPU format b8g8r8a8 lays it out, which is the byte order
 * of the VNC client's pixels too. */
using Pixel = std::array<std::uint8_t, 4>;

/* The guest's ends of one connection: what a guest would be handed. */
struct GuestEndpoints {
    int transport {-1};
    int input {-1};

    GuestEndpoints() = default;
    GuestEndpoints(const GuestEndpoints&) = delete;
    GuestEndpoints& operator=(const GuestEndpoints&) = delete;

    /* The guest has gone: its endpoints are closed, as the service requires
     * before it opens the next connection. */
    void close() noexcept
    {
        if (transport >= 0)
            ::close(transport);
        if (input >= 0)
            ::close(input);
        transport = input = -1;
    }
    ~GuestEndpoints() { close(); }
};

[[nodiscard]] inline std::unique_ptr<GuestEndpoints> takeEndpoints(
    uml::gpu::InProcessGpuService& service)
{
    auto endpoints = std::make_unique<GuestEndpoints>();
    endpoints->transport = descriptorFromName(
        service.takeConnectionName());
    endpoints->input = descriptorFromName(
        service.takeInputConnectionName());
    return endpoints;
}

/* A guest's driver, as far as showing one frame goes: it speaks the
 * vhost-user protocol on `transport`, gives the device one page of memory and
 * sends the control commands of a software scanout. */
class FrameGuest {
public:
    explicit FrameGuest(int transport) : socket_(transport)
    {
        negotiateProtocolFeatures(socket_, protocol::protocolFeatureReplyAck);
        configureMemory(socket_, ring_.descriptor());
        channels_ = startQueue(socket_, ring_);
    }

    /* Shows a frame of `pixel` on the display, as a guest's console does. */
    void show(const Pixel& pixel)
    {
        const std::uint32_t resource = ++resources_;
        auto create = request<protocol::GpuResourceCreate2d>(
            protocol::GpuCommand::resourceCreate2d);
        create.resourceId = resource;
        create.format =
            static_cast<std::uint32_t>(protocol::GpuFormat::b8g8r8a8Unorm);
        create.width = frameExtent;
        create.height = frameExtent;
        send(&create, sizeof(create));

        auto attach = request<protocol::GpuAttachBacking>(
            protocol::GpuCommand::resourceAttachBacking);
        attach.resourceId = resource;
        attach.entryCount = 1;
        const protocol::GpuMemoryEntry entry {
            .address = SplitRing::guestBase + pixelOffset,
            .length = frameBytes,
            .padding = 0,
        };
        std::array<std::uint8_t, sizeof(attach) + sizeof(entry)> attachment {};
        std::memcpy(attachment.data(), &attach, sizeof(attach));
        std::memcpy(attachment.data() + sizeof(attach), &entry, sizeof(entry));
        send(attachment.data(), attachment.size());

        for (std::size_t offset = 0; offset < frameBytes;
             offset += pixel.size()) {
            std::memcpy(
                ring_.memory().data() + pixelOffset + offset,
                pixel.data(),
                pixel.size());
        }
        const protocol::GpuRectangle whole {
            .x = 0, .y = 0, .width = frameExtent, .height = frameExtent};

        auto scanout = request<protocol::GpuSetScanout>(
            protocol::GpuCommand::setScanout);
        scanout.rectangle = whole;
        scanout.scanoutId = 0;
        scanout.resourceId = resource;
        send(&scanout, sizeof(scanout));

        auto transfer = request<protocol::GpuTransferToHost2d>(
            protocol::GpuCommand::transferToHost2d);
        transfer.rectangle = whole;
        transfer.offset = 0;
        transfer.resourceId = resource;
        send(&transfer, sizeof(transfer));

        auto flush = request<protocol::GpuResourceFlush>(
            protocol::GpuCommand::resourceFlush);
        flush.rectangle = whole;
        flush.resourceId = resource;
        send(&flush, sizeof(flush));
    }

private:
    static constexpr std::size_t requestOffset = 0x500;
    static constexpr std::size_t responseOffset = 0x600;
    static constexpr std::size_t pixelOffset = 0x800;
    static constexpr std::uint32_t frameBytes =
        frameExtent * frameExtent * 4U;
    static constexpr std::uint16_t queueSize = 8;

    template<typename Request>
    [[nodiscard]] static Request request(protocol::GpuCommand command)
    {
        Request built {};
        built.header.type = static_cast<std::uint32_t>(command);
        return built;
    }

    /* One control command, run to completion: it must be answered with
     * OK_NODATA. */
    void send(const void* command, std::size_t length)
    {
        require(length <= responseOffset - requestOffset);
        std::memcpy(ring_.memory().data() + requestOffset, command, length);
        ring_.writeDescriptors(requestChain(
            requestOffset,
            static_cast<std::uint32_t>(length),
            responseOffset,
            sizeof(protocol::GpuControlHeader)));
        const std::array<std::uint16_t, 1> head {0};
        ring_.writeHeads(head, published_ % queueSize);
        ++published_;
        ring_.setAvailableIndex(published_);
        writeKick(channels_.kick);
        awaitCall(channels_.call.readEnd.get());
        require(ring_.usedIndex() == published_, "the command did not finish");
        protocol::GpuControlHeader response {};
        std::memcpy(
            &response, ring_.memory().data() + responseOffset,
            sizeof(response));
        require(
            response.type ==
                static_cast<std::uint32_t>(protocol::GpuResponse::okNoData),
            "the device refused a scanout command");
    }

    int socket_;
    SplitRing ring_;
    QueueChannels channels_;
    std::uint16_t published_ {};
    std::uint32_t resources_ {};
};

} // namespace uml::gpu::testing
