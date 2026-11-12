// SPDX-License-Identifier: GPL-2.0-or-later

#include "frame_tap_registry.hpp"

#include <algorithm>

namespace uml::gpu {

FrameTapRegistry& FrameTapRegistry::instance() noexcept
{
    /* Never destroyed. The mirror's Metal completion handler delivers into
     * the registry from a framework thread, and the last one can run after
     * main returns, while the process's static destructors do. */
    static FrameTapRegistry* const registry = new FrameTapRegistry;
    return *registry;
}

void FrameTapRegistry::set(
    FrameTapSink sink,
    void* context,
    FrameTapOptions options) noexcept
{
    options.maximumWidth = std::max<std::uint32_t>(options.maximumWidth, 16);
    options.framesPerSecond =
        std::clamp<std::uint32_t>(options.framesPerSecond, 1, 120);
    /* Taken first, so a delivery in flight finishes before the sink changes
     * and none starts after this returns. */
    std::lock_guard delivery(deliveryMutex_);
    {
        std::lock_guard lock(mutex_);
        sink_ = sink;
        context_ = context;
        options_ = options;
        ++generation_;
        armed_.store(sink != nullptr, std::memory_order_release);
    }
    changed_.notify_all();
}

std::optional<FrameTapRegistry::Armed> FrameTapRegistry::snapshot()
    const noexcept
{
    std::lock_guard lock(mutex_);
    if (sink_ == nullptr)
        return std::nullopt;
    return Armed {.options = options_, .generation = generation_};
}

bool FrameTapRegistry::deliver(
    std::uint64_t generation, void* pixelBuffer) noexcept
{
    std::lock_guard delivery(deliveryMutex_);
    FrameTapSink sink {};
    void* context {};
    {
        std::lock_guard lock(mutex_);
        if (generation != generation_ || sink_ == nullptr)
            return false;
        sink = sink_;
        context = context_;
    }
    sink(context, pixelBuffer);
    return true;
}

void FrameTapRegistry::wake() noexcept
{
    {
        std::lock_guard lock(mutex_);
    }
    changed_.notify_all();
}

void setFrameTap(
    FrameTapSink sink, void* context, FrameTapOptions options) noexcept
{
    FrameTapRegistry::instance().set(sink, context, options);
}

} // namespace uml::gpu
