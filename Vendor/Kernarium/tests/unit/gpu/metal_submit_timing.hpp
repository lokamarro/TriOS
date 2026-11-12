// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <unistd.h>

namespace uml::gpu::test {

struct SubmitTiming {
    uintptr_t device = 0;
    double start_seconds = 0;
    double end_seconds = 0;
    unsigned error = 0;
};

// Pure stream/coverage validator, shared by the native collector regression.
// Only one draw may be outstanding; callback arrival order is never used to
// associate several simultaneously outstanding submissions.
class SubmitTimingLedger {
public:
    static constexpr double wall_tolerance_seconds = 0.000001;

    void begin()
    {
        if (active_ || pending_)
            throw std::runtime_error("submit timing draw already outstanding");
        active_ = true;
        ++expected_;
    }

    void ingest(std::string_view line)
    {
        constexpr std::string_view tag = "KK_SUBMIT_TIMING";
        if (!line.starts_with(tag))
            return;
        ++received_;
        if (!active_ || pending_)
            throw std::runtime_error("unexpected, late or duplicate submit callback");
        line.remove_prefix(tag.size());
        const auto value = [&line](std::string_view key) {
            if (!line.starts_with(key))
                throw std::runtime_error("malformed submit timing record");
            line.remove_prefix(key.size());
            const auto end = line.find(' ');
            const auto result = line.substr(0, end);
            line = end == std::string_view::npos ? std::string_view{} : line.substr(end);
            return result;
        };
        SubmitTiming timing;
        auto device = value(" device=");
        if (!device.starts_with("0x"))
            throw std::runtime_error("invalid submit timing device");
        device.remove_prefix(2);
        number(device, timing.device, 16);
        number(value(" gpu_start_s="), timing.start_seconds);
        number(value(" gpu_end_s="), timing.end_seconds);
        number(value(" error="), timing.error);
        if (!line.empty())
            throw std::runtime_error("trailing submit timing data");
        pending_ = timing;
    }

    bool ready() const { return pending_.has_value(); }

    SubmitTiming complete(double synchronous_wall_seconds)
    {
        if (!active_ || !pending_)
            throw std::runtime_error("missing submit timing callback");
        const auto timing = *pending_;
        const double duration = timing.end_seconds - timing.start_seconds;
        if (!timing.device || (device_ && timing.device != device_) || timing.error ||
            !std::isfinite(timing.start_seconds) || !std::isfinite(timing.end_seconds) ||
            timing.start_seconds <= 0 || timing.end_seconds <= timing.start_seconds ||
            timing.start_seconds < previous_end_ || !std::isfinite(synchronous_wall_seconds) ||
            synchronous_wall_seconds <= 0 ||
            duration > synchronous_wall_seconds + wall_tolerance_seconds)
            throw std::runtime_error("invalid whole-submit timing or device association");
        device_ = timing.device;
        previous_end_ = timing.end_seconds;
        pending_.reset();
        active_ = false;
        ++matched_;
        return timing;
    }

    void finish() const
    {
        if (active_ || pending_ || received_ != expected_ || matched_ != expected_)
            throw std::runtime_error("incomplete whole-submit callback coverage");
    }

    unsigned matched() const { return matched_; }

private:
    template <typename T>
    static void number(std::string_view text, T &destination)
    {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), destination);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
            throw std::runtime_error("invalid submit timing number");
    }
    static void number(std::string_view text, uintptr_t &destination, int base)
    {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), destination, base);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
            throw std::runtime_error("invalid submit timing device number");
    }
    bool active_ = false;
    uintptr_t device_ = 0;
    double previous_end_ = 0;
    unsigned expected_ = 0, received_ = 0, matched_ = 0;
    std::optional<SubmitTiming> pending_;
};

// Test-only process-wide stderr observer. Every byte is forwarded unchanged to
// the original stderr. Keep it alive around all reference and measured draws;
// finish() drains the pipe and audits callbacks received after the final draw.
class SubmitTimingCollector {
public:
    SubmitTimingCollector()
    {
        std::fflush(stderr);
        int descriptors[2];
        if (pipe(descriptors) != 0)
            throw std::runtime_error("cannot create submit timing pipe");
        read_fd_ = descriptors[0];
        original_stderr_ = dup(STDERR_FILENO);
        if (original_stderr_ < 0 || dup2(descriptors[1], STDERR_FILENO) < 0) {
            close(descriptors[1]);
            close(read_fd_);
            if (original_stderr_ >= 0)
                close(original_stderr_);
            throw std::runtime_error("cannot observe submit timing stderr");
        }
        close(descriptors[1]);
        try {
            reader_ = std::thread([this] { read_stream(); });
        } catch (...) {
            dup2(original_stderr_, STDERR_FILENO);
            close(original_stderr_);
            close(read_fd_);
            throw;
        }
    }

    SubmitTimingCollector(const SubmitTimingCollector &) = delete;
    SubmitTimingCollector &operator=(const SubmitTimingCollector &) = delete;
    ~SubmitTimingCollector() { close_stream(); }

    void begin()
    {
        const std::lock_guard guard(lock_);
        check_failure();
        ledger_.begin();
    }

    SubmitTiming complete(double synchronous_wall_seconds,
                          std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        std::unique_lock guard(lock_);
        if (!changed_.wait_for(guard, timeout,
                [this] { return !failure_.empty() || ledger_.ready(); }))
            throw std::runtime_error("missing whole-submit callback (timeout)");
        check_failure();
        return ledger_.complete(synchronous_wall_seconds);
    }

    unsigned finish()
    {
        close_stream();
        const std::lock_guard guard(lock_);
        check_failure();
        ledger_.finish();
        return ledger_.matched();
    }

private:
    void check_failure() const
    {
        if (!failure_.empty())
            throw std::runtime_error(failure_);
    }

    void fail(std::string message)
    {
        const std::lock_guard guard(lock_);
        if (failure_.empty())
            failure_ = std::move(message);
        changed_.notify_all();
    }

    void read_stream()
    {
        std::string pending;
        std::array<char, 4096> bytes{};
        for (;;) {
            const auto count = read(read_fd_, bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0) {
                if (count < 0)
                    fail("submit timing pipe read failed");
                if (pending.starts_with("KK_SUBMIT_TIMING"))
                    fail("truncated submit timing record");
                return;
            }
            ssize_t forwarded = 0;
            while (forwarded < count) {
                const auto written = write(original_stderr_, bytes.data() + forwarded,
                                           size_t(count - forwarded));
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0) {
                    fail("cannot preserve original stderr");
                    return;
                }
                forwarded += written;
            }
            for (ssize_t index = 0; index < count; ++index) {
                const char byte = bytes[size_t(index)];
                if (byte == '\n') {
                    try {
                        const std::lock_guard guard(lock_);
                        ledger_.ingest(pending);
                        changed_.notify_all();
                    } catch (const std::exception &error) {
                        fail(error.what());
                    }
                    pending.clear();
                } else {
                    pending += byte;
                    if (pending.size() > 65536) {
                        fail("stderr line exceeds submit collector bound");
                        pending.clear();
                    }
                }
            }
        }
    }

    void close_stream()
    {
        if (original_stderr_ < 0)
            return;
        std::fflush(stderr);
        if (dup2(original_stderr_, STDERR_FILENO) < 0) {
            fail("cannot restore original stderr");
            close(STDERR_FILENO);
        }
        if (reader_.joinable())
            reader_.join();
        close(read_fd_);
        close(original_stderr_);
        read_fd_ = original_stderr_ = -1;
    }

    int read_fd_ = -1, original_stderr_ = -1;
    std::thread reader_;
    std::mutex lock_;
    std::condition_variable changed_;
    SubmitTimingLedger ledger_;
    std::string failure_;
};

} // namespace uml::gpu::test
