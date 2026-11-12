// SPDX-License-Identifier: GPL-2.0-or-later
// A stateful V4L2 encoder node written in memory, for testing the encoder and
// the VA-API layer above it without a guest kernel.
//
// It keeps the queue discipline of the real node: raw frames go in on the
// OUTPUT queue, coded frames come out on the CAPTURE queue in submission order
// with the timestamp of the frame they encode, a raw buffer completes as soon
// as the device has taken the frame, and the device defers raw completion when
// it already holds too many coded frames. The coded data is a small Annex B
// stream whose content depends on the pixels the device was given, so a test
// can tell that the right picture arrived.
#pragma once

#include <linux/videodev2.h>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../v4l2_io.hpp"
#include "../v4l2_encoder.hpp"

namespace uml::va::test {

class FakeEncoderNode final : public V4l2Io {
public:
    struct Options {
        bool nv12m {true};
        bool nv12 {true};
        bool hevc {true};
        // Rows of a raw frame start this many bytes apart, a multiple of it.
        unsigned pitchAlign {64};
        // The device defers the completion of raw buffers while it holds this
        // many coded frames that nobody has collected.
        unsigned codedLimit {8};
        // Coded frames are delivered only by deliver(), to test waiting.
        bool holdCoded {false};
        // CAPTURE sizeimage; zero is max(w * h * 3 / 2, 1 MiB).
        std::uint32_t codedSize {};
        // Frames are emitted as key frames whatever was asked.
        bool ignoreForceKey {false};
        // The node publishes no controls at all.
        bool noControls {false};
        // Which bitrate modes (bit 0 variable, 1 constant, 2 constant quality)
        // and H.264 profile menu items the node offers.
        unsigned bitrateModeMask {0x7};
        std::uint64_t h264ProfileMask {(1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (1ull << 17)};
        // The node refuses these requests.
        bool failStreamOn {false};
        bool failSetFormat {false};
        // The coded frame with this ordinal (counted from 0) carries the error flag.
        int errorFrame {-1};
    };

    FakeEncoderNode();
    explicit FakeEncoderNode(const Options& options);

    static constexpr const char* kPath = "/dev/fake-encoder";

    // V4l2Io.
    int open(const char* path) override;
    int close(int fd) override;
    int ioctl(int fd, unsigned long request, void* argument) override;
    void* mmap(int fd, std::size_t length, std::int64_t offset) override;
    int munmap(void* address, std::size_t length) override;
    int poll(pollfd* fds, unsigned long count, int timeoutMilliseconds) override;

    // What a test can look at.
    struct ControlSet {
        std::uint32_t id {};
        std::int64_t value {};
        bool streaming {};
        int result {};
    };
    struct Frame {
        std::uint64_t timestamp {};
        unsigned width {};
        unsigned height {};
        std::uint64_t checksum {};
        bool key {};
    };
    std::vector<ControlSet> controlLog;
    std::vector<Frame> frames;
    int opens {};
    int closes {};
    // Refuses to open the node, as a sandbox does.
    bool denyOpen {};
    int streamOns {};
    int streamOffs {};
    int requestBuffersFree {};
    std::uint32_t outputFormat {};
    unsigned outputPlanes {};
    unsigned outputWidth {};
    unsigned outputHeight {};
    unsigned outputPitch[2] {};
    std::uint32_t codedFormat {};
    unsigned frameRateNumerator {};
    unsigned frameRateDenominator {};
    // The current value of a control, as the device holds it.
    std::int64_t control(std::uint32_t id) const;
    // Lets held coded frames through, oldest first. Returns how many moved.
    unsigned deliver(unsigned count);
    // Coded frames the device has produced and nobody has collected.
    unsigned codedWaiting() const;

    // The checksum the device computes over a picture's visible area, so that a
    // test can predict it from the frame it submitted.
    static std::uint64_t checksum(const std::uint8_t* luma, std::size_t lumaPitch, const std::uint8_t* chroma,
                                  std::size_t chromaPitch, unsigned width, unsigned height);

    // The controls this node publishes.
    const std::map<std::uint32_t, ControlInfo>& controlTable() const { return table_; }

private:
    struct Buffer {
        std::vector<std::vector<std::uint8_t>> planes;
        bool queued {};
        std::uint64_t timestamp {};
        std::uint32_t bytesused {};
        std::uint32_t flags {};
        std::uint64_t frameChecksum {};
        bool ready {};
    };
    struct Coded {
        std::vector<std::uint8_t> data;
        std::uint64_t timestamp {};
        bool key {};
        bool error {};
    };
    struct Session {
        int fd {};
        v4l2_format output {};
        v4l2_format capture {};
        std::map<std::uint32_t, std::int64_t> values;
        std::vector<Buffer> raw;
        std::vector<Buffer> coded;
        bool rawOn {};
        bool codedOn {};
        std::deque<unsigned> rawDone;
        std::deque<unsigned> codedFree;
        std::deque<unsigned> codedDone;
        std::deque<Coded> internal;
        std::deque<unsigned> rawWaiting;
        std::uint64_t ordinal {};
        std::uint64_t sinceKey {};
        bool forceKey {};
        bool started {};
    };

    Session* find(int fd);
    void buildTable();
    void advance(Session& session);
    void encodeOne(Session& session, unsigned index);
    bool moveCoded(Session& session);
    bool encodeWaiting(Session& session);
    int setExtControls(Session& session, v4l2_ext_controls& controls);
    int queryExtControl(v4l2_query_ext_ctrl& query);
    int setFormat(Session& session, v4l2_format& format, bool apply);
    int requestBuffers(Session& session, v4l2_requestbuffers& request);
    int queueBuffer(Session& session, v4l2_buffer& buffer);
    int dequeueBuffer(Session& session, v4l2_buffer& buffer);
    int queryBuffer(Session& session, v4l2_buffer& buffer);
    void stop(Session& session, unsigned type);

    Options options_;
    std::map<std::uint32_t, ControlInfo> table_;
    std::map<std::uint32_t, bool> dynamic_;
    std::map<int, std::unique_ptr<Session>> sessions_;
    int next_ {1000};
    int focus_ {};
};

}
