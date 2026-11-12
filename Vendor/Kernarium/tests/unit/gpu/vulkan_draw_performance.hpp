// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "vulkan_draw_test.hpp"
#include "metal_submit_timing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace uml::gpu::test {

inline constexpr unsigned draw_performance_warmups = 2;
inline constexpr unsigned draw_performance_samples = 7;

struct DrawPerformance {
    double gpu_nanoseconds;
    double record_nanoseconds;
};

class DrawPerformanceSession {
public:
    DrawPerformanceSession(const DrawContext &context, bool whole_submit)
        : whole_submit_(whole_submit)
    {
        if (whole_submit_) {
            if (context.capabilities().driver_id != VK_DRIVER_ID_MESA_KOSMICKRISP)
                throw std::runtime_error("whole-submit feedback requires native KosmicKrisp");
            const char *debug = std::getenv("MESA_KK_DEBUG");
            if (!debug || std::string_view(debug).find("submit_timing") == std::string_view::npos)
                throw std::runtime_error("whole-submit mode requires MESA_KK_DEBUG=submit_timing");
            collector_ = std::make_unique<SubmitTimingCollector>();
        }
        std::printf("PERF_TIMING source=%s cpu_scope=instrumented-command-recording warmups=%u samples=%u\n",
                    source(), draw_performance_warmups, draw_performance_samples);
    }

    const char *source() const
    {
        return whole_submit_ ? "metal-whole-submit" : "vulkan-render-queries";
    }

    DrawResult draw(DrawContext &context, const DrawConfig &config,
                    const std::vector<uint8_t> &cache, const char *phase)
    {
        auto actual = config;
        if (whole_submit_) {
            actual.timestamp = false;
            collector_->begin();
        }
        const auto begin = std::chrono::steady_clock::now();
        auto result = context.draw(actual, cache);
        const double wall_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - begin).count();
        if (collector_) {
            const auto timing = collector_->complete(wall_seconds);
            result.gpu_nanoseconds = (timing.end_seconds - timing.start_seconds) * 1e9;
        }
        ++draws_;
        const bool timed = config.timestamp || whole_submit_;
        const bool valid = !timed ||
            (std::isfinite(result.gpu_nanoseconds) && result.gpu_nanoseconds > 0 &&
             result.gpu_nanoseconds <= (wall_seconds + SubmitTimingLedger::wall_tolerance_seconds) * 1e9 &&
             std::isfinite(result.record_nanoseconds) && result.record_nanoseconds > 0);
        std::printf("PERF_DRAW id=%u source=%s phase=%s width=%u vertices=%u instances=%u repeat=%u timed=%u gpu_ns=%.17g recording_ns=%.17g synchronous_wall_ns=%.17g valid=%u\n",
                    draws_, source(), phase, config.width, config.vertex_count,
                    config.instance_count, config.repeat_count, unsigned(timed),
                    result.gpu_nanoseconds, result.record_nanoseconds,
                    wall_seconds * 1e9, unsigned(valid));
        std::fflush(stdout);
        if (!valid)
            throw std::runtime_error("invalid performance interval (including warmup)");
        return result;
    }

    DrawResult reference(DrawContext &context, const DrawConfig &config)
    {
        return draw(context, config, {}, "reference");
    }

    void finish()
    {
        if (draws_ == 0 || (collector_ && collector_->finish() != draws_))
            throw std::runtime_error("whole-submit draw/callback coverage mismatch");
        std::printf("PERF_COVERAGE source=%s draws=%u callbacks=%u complete=1\n",
                    source(), draws_, collector_ ? draws_ : 0);
        std::fflush(stdout);
    }

private:
    bool whole_submit_;
    unsigned draws_ = 0;
    std::unique_ptr<SubmitTimingCollector> collector_;
};

inline void print_draw_performance_device(const DrawContext &context)
{
    const auto &capabilities = context.capabilities();
    std::printf("GPU performance device=\"%s\" driver=\"%s\" warmups=%u samples=%u\n",
                capabilities.device_name.c_str(), capabilities.driver_name.c_str(),
                draw_performance_warmups, draw_performance_samples);
    std::fflush(stdout);
}

// Render queries and whole-Metal-submit feedback are distinct GPU quantities.
// CPU timing remains the instrumented draw-command recording interval. Every
// reference, warmup and retained sample keeps its correctness oracle; validity
// is never filtered. Only warmups are excluded from the reported medians.
template <typename Verify>
DrawPerformance measure_draw_performance(DrawContext &context,
                                         const DrawConfig &config, Verify verify,
                                         DrawPerformanceSession &session)
{
    if (!config.timestamp || config.repeat_count == 0)
        throw std::runtime_error("performance draws require timestamps and a nonzero repeat count");
    std::array<double, draw_performance_samples> gpu{}, record{};
    std::vector<uint8_t> cache;
    for (unsigned sample = 0;
         sample < draw_performance_warmups + draw_performance_samples; ++sample) {
        auto result = session.draw(context, config, cache,
            sample < draw_performance_warmups ? "warmup" : "retained");
        verify(result);
        cache = std::move(result.pipeline_cache);
        if (sample >= draw_performance_warmups) {
            if (!std::isfinite(result.gpu_nanoseconds) || result.gpu_nanoseconds <= 0 ||
                !std::isfinite(result.record_nanoseconds) || result.record_nanoseconds <= 0)
                throw std::runtime_error("invalid performance timing at sample " +
                    std::to_string(sample) + ": GPU=" + std::to_string(result.gpu_nanoseconds) +
                    " ns, recording=" + std::to_string(result.record_nanoseconds) + " ns");
            const unsigned index = sample - draw_performance_warmups;
            gpu[index] = result.gpu_nanoseconds / config.repeat_count;
            record[index] = result.record_nanoseconds / config.repeat_count;
        }
    }
    std::sort(gpu.begin(), gpu.end());
    std::sort(record.begin(), record.end());
    return {gpu[gpu.size() / 2], record[record.size() / 2]};
}

} // namespace uml::gpu::test
