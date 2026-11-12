// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "video_decoder.hpp"
#include "video_encoder.hpp"
#include "vhost_user_server.hpp"
#include "virtio_video_device.hpp"
#include "virtio_video_encoder_device.hpp"

#include <utility>

namespace uml::video {

std::unique_ptr<uml::gpu::InProcessVhostUserService> makeInProcessService(
    std::shared_ptr<uml::virtio::SharedMemoryRegions> regions)
{
    return std::make_unique<uml::gpu::InProcessVhostUserService>(
        "Virtio video", "uml-video",
        [] {
            return std::make_unique<VirtioVideoDevice>(
                uml::platform::videoDecodeCapabilities(),
                uml::platform::makeVideoDecoder);
        },
        std::move(regions));
}

std::unique_ptr<uml::gpu::InProcessVhostUserService> makeInProcessEncoderService(
    std::shared_ptr<uml::virtio::SharedMemoryRegions> regions)
{
    /* Discovery runs on a background thread from here, so the capability
     * query the guest sends at probe rarely waits for it. */
    uml::platform::prepareVideoEncodeCapabilities();
    return std::make_unique<uml::gpu::InProcessVhostUserService>(
        "Virtio video encoder", "uml-video-encoder",
        [] {
            return std::make_unique<VirtioVideoEncoderDevice>(
                uml::platform::videoEncodeCapabilities(),
                uml::platform::makeVideoEncoder);
        },
        std::move(regions));
}

} // namespace uml::video
