// SPDX-License-Identifier: GPL-2.0-or-later

#include "capture_source.hpp"

namespace uml::audio {
namespace {

class SilentCaptureSource final : public CaptureSource {
public:
    void configure(const CaptureParameters&) noexcept override {}
    void prepare() noexcept override {}
    void start() noexcept override {}
    void stop() noexcept override {}
    void release() noexcept override {}
    void reset() noexcept override {}

    [[nodiscard]] std::size_t read(std::span<std::uint8_t>) noexcept override
    {
        return 0;
    }
};

} // namespace

std::unique_ptr<CaptureSource> makeSilentCaptureSource()
{
    return std::make_unique<SilentCaptureSource>();
}

} // namespace uml::audio
