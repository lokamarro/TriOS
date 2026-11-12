// SPDX-License-Identifier: GPL-2.0-or-later
// Host-independent checks for the parts of the VA-API backend that do not need
// a decoder: the VP9 header reader, the superframe assembly the hidden-frame
// rule depends on, and the pixel paths. Runs natively on any Linux build host.
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <va/va.h>

#include "../convert.hpp"
#include "../storage.hpp"
#include "../v4l2_decoder.hpp"
#include "../vp9.hpp"
#include "test_support.hpp"

namespace {

using uml::va::test::check;

// Writes VP9 uncompressed headers bit by bit, which is the only way to build
// the fixtures this parser has to read.
class HeaderWriter {
public:
    void bit(unsigned value)
    {
        if (count_ == 0) { bytes_.push_back(0); count_ = 8; }
        --count_;
        if (value) bytes_.back() = std::uint8_t(bytes_.back() | (1u << count_));
    }
    void bits(unsigned value, unsigned width)
    {
        for (unsigned index = width; index-- > 0;) bit((value >> index) & 1u);
    }
    void syncCode() { bits(0x49, 8); bits(0x83, 8); bits(0x42, 8); }
    std::vector<std::uint8_t> take()
    {
        // A real frame carries a compressed header and tile data after this;
        // a few trailing bytes stand in for them.
        bytes_.insert(bytes_.end(), {0x00, 0x00, 0x00, 0x00});
        return bytes_;
    }

private:
    std::vector<std::uint8_t> bytes_;
    unsigned count_ {};
};

std::vector<std::uint8_t> keyFrame(unsigned width, unsigned height, bool show = true)
{
    HeaderWriter writer;
    writer.bits(2, 2);       // frame_marker
    writer.bit(0);           // profile_low_bit
    writer.bit(0);           // profile_high_bit
    writer.bit(0);           // show_existing_frame
    writer.bit(0);           // frame_type: key
    writer.bit(show ? 1 : 0);
    writer.bit(0);           // error_resilient_mode
    writer.syncCode();
    writer.bits(2, 3);       // color_space: BT.709
    writer.bit(0);           // color_range
    writer.bits(width - 1, 16);
    writer.bits(height - 1, 16);
    writer.bit(0);           // render_and_frame_size_different
    return writer.take();
}

std::vector<std::uint8_t> interFrame(bool show, unsigned refreshFlags)
{
    HeaderWriter writer;
    writer.bits(2, 2);
    writer.bit(0);
    writer.bit(0);
    writer.bit(0);           // show_existing_frame
    writer.bit(1);           // frame_type: inter
    writer.bit(show ? 1 : 0);
    writer.bit(0);           // error_resilient_mode
    if (!show) writer.bit(0);// intra_only
    writer.bits(0, 2);       // reset_frame_context
    writer.bits(refreshFlags, 8);
    for (unsigned index = 0; index < 3; ++index) { writer.bits(index, 3); writer.bit(0); }
    writer.bit(1);           // found_ref for the first reference
    writer.bit(0);           // render_and_frame_size_different
    return writer.take();
}

void testHeaders()
{
    uml::va::Vp9Context context;
    const auto key = keyFrame(1920, 1200);
    auto header = context.parse(key);
    check(header.valid, "a key frame header parses");
    check(header.keyFrame && header.showFrame, "a key frame is a shown key frame");
    check(header.width == 1920 && header.height == 1200, "a key frame carries its size");
    check(header.refreshFrameFlags == 0xff, "a key frame refreshes every slot");
    check(header.profile == 0, "profile 0 is read");

    const auto hidden = interFrame(false, 0x04);
    header = context.parse(hidden);
    check(header.valid, "a hidden inter frame parses");
    check(!header.showFrame && !header.keyFrame, "a hidden frame is not shown");
    check(header.refreshFrameFlags == 0x04, "the refresh mask is read");
    check(header.width == 1920 && header.height == 1200, "an inter frame takes its size from a reference");

    HeaderWriter writer;
    writer.bits(2, 2);
    writer.bit(0);
    writer.bit(0);
    writer.bit(1);          // show_existing_frame
    writer.bits(2, 3);      // frame_to_show_map_idx
    const auto existing = writer.take();
    header = context.parse(existing);
    check(header.valid && header.showExistingFrame, "show_existing_frame parses");
    check(header.frameToShowMapIndex == 2, "the shown slot is read");
    check(header.width == 1920, "the shown slot keeps its size");

    const std::uint8_t truncated[] {0x82};
    header = context.parse(truncated);
    check(!header.valid, "a truncated header is rejected");
    const std::uint8_t wrongMarker[] {0x00, 0x00, 0x00};
    header = context.parse(wrongMarker);
    check(!header.valid, "a frame without the VP9 marker is rejected");
}

void testShowExistingByte()
{
    for (unsigned slot = 0; slot < 8; ++slot) {
        const std::uint8_t byte = uml::va::vp9ShowExistingFrameByte(slot);
        uml::va::Vp9Context context;
        const auto key = keyFrame(64, 64);
        context.parse(key);
        const std::uint8_t frame[] {byte};
        const auto header = context.parse(frame);
        check(header.valid, "the synthesised show_existing_frame parses");
        check(header.showExistingFrame, "the synthesised frame shows an existing one");
        check(header.frameToShowMapIndex == slot, "the synthesised frame names the right slot");
    }
}

void testSuperframes()
{
    const auto first = keyFrame(320, 240);
    const std::uint8_t tail = uml::va::vp9ShowExistingFrameByte(3);
    const std::span<const std::uint8_t> parts[2] {first, std::span<const std::uint8_t>(&tail, 1)};
    const auto packet = uml::va::vp9BuildSuperframe(std::span<const std::span<const std::uint8_t>>(parts, 2));
    check(!packet.empty(), "a superframe is built");
    const auto split = uml::va::vp9SuperframeParts(packet);
    check(split.size() == 2, "the superframe splits back into two frames");
    if (split.size() == 2) {
        check(split[0].size() == first.size(), "the first frame keeps its length");
        check(split[1].size() == 1, "the trailing frame is one byte");
        check(std::memcmp(split[0].data(), first.data(), first.size()) == 0, "the first frame is unchanged");
        check(split[1][0] == tail, "the trailing byte is unchanged");
    }
    const auto single = uml::va::vp9SuperframeParts(first);
    check(single.size() == 1 && single[0].size() == first.size(), "a plain frame is its own only part");

    // A byte that merely looks like a superframe marker must not split a frame.
    std::vector<std::uint8_t> pretend {0x82, 0x49, 0xc1};
    const auto pretendParts = uml::va::vp9SuperframeParts(pretend);
    check(pretendParts.size() == 1, "a false marker leaves the frame whole");
}

void testPixels()
{
    constexpr unsigned width = 32;
    constexpr unsigned height = 16;
    std::vector<std::uint8_t> luma(std::size_t(width) * height);
    std::vector<std::uint8_t> chroma(std::size_t(width) * height / 2);
    for (std::size_t index = 0; index < luma.size(); ++index) luma[index] = std::uint8_t(index & 0xff);
    for (std::size_t index = 0; index < chroma.size(); ++index) chroma[index] = 128;

    std::vector<std::uint8_t> copyLuma(luma.size(), 0);
    std::vector<std::uint8_t> copyChroma(chroma.size(), 0);
    const uml::va::ConstPlaneView source[2] {{luma.data(), width}, {chroma.data(), width}};
    const uml::va::PlaneView destination[2] {{copyLuma.data(), width}, {copyChroma.data(), width}};
    uml::va::copyNv12(source, destination, width, height);
    check(copyLuma == luma && copyChroma == chroma, "an NV12 copy is exact");

    // A wider destination pitch must not disturb the payload.
    const unsigned pitch = width + 16;
    std::vector<std::uint8_t> wideLuma(std::size_t(pitch) * height, 0xaa);
    std::vector<std::uint8_t> wideChroma(std::size_t(pitch) * height / 2, 0xaa);
    const uml::va::PlaneView wide[2] {{wideLuma.data(), pitch}, {wideChroma.data(), pitch}};
    uml::va::copyNv12(source, wide, width, height);
    bool rowsMatch = true;
    for (unsigned row = 0; row < height; ++row)
        rowsMatch = rowsMatch && std::memcmp(wideLuma.data() + std::size_t(row) * pitch,
                                             luma.data() + std::size_t(row) * width, width) == 0;
    check(rowsMatch, "an NV12 copy honours a wider pitch");

    std::vector<std::uint8_t> argb(std::size_t(width) * height * 4, 0);
    const uml::va::PlaneView out {argb.data(), std::size_t(width) * 4};
    uml::va::convertNv12ToArgb(source, {0, 0, width, height}, out, {0, 0, width, height}, true);
    // Neutral chroma keeps the three colour channels equal, and the alpha is
    // opaque.
    bool neutral = true;
    for (std::size_t pixel = 0; pixel < std::size_t(width) * height; ++pixel) {
        neutral = neutral && argb[4 * pixel] == argb[4 * pixel + 1] && argb[4 * pixel + 1] == argb[4 * pixel + 2];
        neutral = neutral && argb[4 * pixel + 3] == 0xff;
    }
    check(neutral, "neutral chroma converts to grey with opaque alpha");
    // Luma 0..16 is below the studio-swing floor and clamps to black, so a
    // sample well inside the range is the one that has to be brighter.
    check(argb[4 * 200] > argb[4 * 20], "brighter luma converts to a brighter pixel");
    check(argb[4 * 5] == 0, "luma below the studio floor is black");
}

void testLayout()
{
    unsigned planes = 0;
    std::size_t pitch[4] {};
    std::size_t offset[4] {};
    std::size_t size = 0;
    check(uml::va::planeLayout(VA_FOURCC_NV12, 1920, 1200, planes, pitch, offset, size), "NV12 has a layout");
    check(planes == 2, "NV12 has two planes");
    check(pitch[0] == 1920 && offset[0] == 0, "the NV12 luma plane is first and tightly packed");
    check(offset[1] == std::size_t(1920) * 1200, "the NV12 chroma plane follows the luma plane");
    check(size == std::size_t(1920) * 1200 * 3 / 2, "an NV12 surface is one and a half times its luma");
    check(uml::va::planeLayout(VA_FOURCC_ARGB, 1280, 720, planes, pitch, offset, size), "ARGB has a layout");
    check(planes == 1 && pitch[0] == 1280 * 4, "ARGB is one packed plane");
    check(!uml::va::planeLayout(VA_FOURCC_P010, 64, 64, planes, pitch, offset, size), "P010 is refused for now");
}

void testCapturedPlaneBounds()
{
    using uml::va::capturedPlaneFits;
    constexpr std::size_t luma = std::size_t(1920) * 1080;
    check(capturedPlaneFits(V4L2_PIX_FMT_NV12M, 0, 1920, 1080, 1920, 0, luma), "a tight NV12 luma plane fits");
    check(!capturedPlaneFits(V4L2_PIX_FMT_NV12M, 0, 1920, 1080, 1920, 0, luma - 1),
          "an NV12 luma plane one byte short does not fit");
    check(!capturedPlaneFits(V4L2_PIX_FMT_NV12M, 0, 1920, 1080, 1920, 1, luma),
          "a data offset that pushes the luma rows past the plane does not fit");
    check(!capturedPlaneFits(V4L2_PIX_FMT_NV12M, 0, 1920, 1080, 2048, 0, luma),
          "a stride wider than the mapped rows does not fit");
    check(capturedPlaneFits(V4L2_PIX_FMT_NV12M, 1, 1920, 1080, 1920, 0, luma / 2),
          "a tight NV12 chroma plane fits");
    check(!capturedPlaneFits(V4L2_PIX_FMT_NV12M, 1, 1920, 1080, 1920, 0, luma / 2 - 1),
          "an NV12 chroma plane one byte short does not fit");
    // Odd sizes round the chroma up to whole sample pairs and rows.
    check(capturedPlaneFits(V4L2_PIX_FMT_NV12M, 1, 1921, 1081, 1922, 0, std::size_t(1922) * 541),
          "odd NV12 chroma fits in whole sample pairs");
    check(!capturedPlaneFits(V4L2_PIX_FMT_NV12M, 1, 1921, 1081, 1922, 0, std::size_t(1922) * 541 - 1),
          "odd NV12 chroma needs its last sample pair");
    // Only the bytes a row reads count, so the last row may end before its
    // pitch does.
    check(capturedPlaneFits(V4L2_PIX_FMT_ABGR32, 0, 100, 10, 512, 0, 512 * 9 + 400),
          "a packed picture whose last row ends short of its pitch fits");
    check(!capturedPlaneFits(V4L2_PIX_FMT_ABGR32, 0, 100, 10, 512, 0, 512 * 9 + 399),
          "a packed picture one byte short does not fit");
    check(capturedPlaneFits(V4L2_PIX_FMT_ABGR32, 1, 100, 10, 512, 0, 0), "a packed picture reads no second plane");
    check(!capturedPlaneFits(V4L2_PIX_FMT_ABGR32, 0, 100, 10, 512, 4097, 4096),
          "a data offset past the plane does not fit");
    check(!capturedPlaneFits(V4L2_PIX_FMT_ABGR32, 0, 100, 2, SIZE_MAX, 0, 4096),
          "a stride that overflows does not fit");
}

void testMemoryStorage()
{
    uml::va::selectStorage("memory");
    std::string error;
    auto buffer = uml::va::PixelBuffer::allocate(VA_FOURCC_NV12, 64, 48, error);
    check(buffer != nullptr, "a memory surface allocates");
    if (!buffer) return;
    check(buffer->planes() == 2, "a memory NV12 surface has two planes");
    check(buffer->map() != nullptr, "a memory surface maps");
    const int fd = buffer->exportDescriptor();
    check(fd >= 0, "a memory surface exports a descriptor");
    if (fd >= 0) {
        check(buffer->identity() != 0, "an exported surface has an identity");
        ::close(fd);
    }
    buffer->map()[0] = 0x5a;
    check(buffer->map()[0] == 0x5a, "a memory surface keeps what is written to it");
}

// The decode format policy: which capture format a session asks the device
// for, given what the device offers and what the client is doing.
void testCapturePolicy()
{
    using uml::va::RgbCapture;
    check(uml::va::rgbCaptureRequest(nullptr) == RgbCapture::automatic,
          "an unset request leaves the policy automatic");
    check(uml::va::rgbCaptureRequest("off") == RgbCapture::off, "off is honoured");
    check(uml::va::rgbCaptureRequest("nv12") == RgbCapture::off, "nv12 means off");
    for (const char* request : {"force", "1", "rgb"})
        check(uml::va::rgbCaptureRequest(request) == RgbCapture::automatic,
              "a retired force request keeps the automatic policy");
    check(uml::va::rgbCaptureRequest("nonsense") == RgbCapture::automatic,
          "an unrecognised request leaves the policy automatic");

    check(!uml::va::useRgbCapture(RgbCapture::automatic, false),
          "a device that does not offer packed RGB is never asked for it");
    check(!uml::va::useRgbCapture(RgbCapture::off, true), "off never asks for packed RGB");
    check(uml::va::useRgbCapture(RgbCapture::automatic, true),
          "automatic asks for packed RGB, which makes a browser's mandatory blit a copy");

    using uml::va::CaptureMemory;
    check(uml::va::captureMemoryRequest(nullptr) == CaptureMemory::automatic,
          "an unset capture request leaves the choice to the probe");
    check(uml::va::captureMemoryRequest("") == CaptureMemory::automatic,
          "an empty capture request leaves the choice to the probe");
    check(uml::va::captureMemoryRequest("device") == CaptureMemory::device,
          "a capture request can leave the decoder its own buffers");
    check(uml::va::captureMemoryRequest("lend") == CaptureMemory::automatic,
          "a request to lend is the automatic choice, which already lends wherever it can");
    check(uml::va::captureMemoryRequest("nonsense") == CaptureMemory::automatic,
          "an unrecognised capture request leaves the choice to the probe");
}

// A planar surface is carried by one packed buffer, because a client that
// imports it may only be handed one object.
void testCapturePool()
{
    // The pool behind the decoder's imported capture queue: a buffer goes back
    // when the surface that showed its picture lets go, and never before.
    std::vector<std::shared_ptr<uml::va::PixelBuffer>> pool;
    check(!uml::va::takeReusable(pool, VA_FOURCC_NV12, 64, 64), "an empty pool hands out nothing");

    std::string error;
    std::shared_ptr<uml::va::PixelBuffer> first = uml::va::PixelBuffer::allocate(VA_FOURCC_NV12, 64, 64, error);
    check(first != nullptr, "the pool's buffers can be allocated");
    auto* firstAddress = first.get();
    uml::va::returnToPool(pool, first);  // a copy: the caller keeps its own
    check(pool.size() == 1, "a returned buffer is kept");
    check(first != nullptr, "returning a buffer by value leaves the caller's copy alone");

    // The caller still holds `first`, which is exactly the case of a surface
    // still showing the picture: the pool must not hand it out.
    check(!uml::va::takeReusable(pool, VA_FOURCC_NV12, 64, 64),
          "a buffer something else still refers to is not reused");
    first.reset();
    check(!uml::va::takeReusable(pool, VA_FOURCC_NV12, 32, 32), "a buffer of another size is not reused");
    check(!uml::va::takeReusable(pool, VA_FOURCC_ARGB, 64, 64), "a buffer of another format is not reused");
    auto again = uml::va::takeReusable(pool, VA_FOURCC_NV12, 64, 64);
    check(again.get() == firstAddress, "a buffer nothing refers to comes back");
    check(!uml::va::takeReusable(pool, VA_FOURCC_NV12, 64, 64), "a buffer is only handed out once");

    // A returned buffer reuses an empty slot before the pool grows.
    uml::va::returnToPool(pool, std::move(again));
    check(pool.size() == 1, "an empty slot is refilled before the pool grows");
    for (int index = 0; index < 4; ++index) {
        std::shared_ptr<uml::va::PixelBuffer> extra = uml::va::PixelBuffer::allocate(VA_FOURCC_NV12, 64, 64, error);
        uml::va::returnToPool(pool, std::move(extra), 3);
    }
    check(pool.size() == 3, "the pool is bounded");
}

void testPlanarCarrier()
{
    unsigned carrierWidth = 0;
    unsigned carrierHeight = 0;
    std::size_t pitch = 0;
    check(uml::va::planarCarrier(VA_FOURCC_NV12, 1920, 1200, carrierWidth, carrierHeight, pitch),
          "NV12 is carried");
    check(pitch == 1920, "a 1920 wide surface needs no row padding");
    check(carrierWidth == 480, "the carrier is a quarter as wide in 32-bit pixels");
    check(carrierHeight == 1800, "the carrier holds the luma and the chroma rows");
    check(pitch * carrierHeight == 3456000, "the carrier is exactly the NV12 size");

    check(uml::va::planarCarrier(VA_FOURCC_NV12, 1918, 1080, carrierWidth, carrierHeight, pitch),
          "an unaligned width is carried too");
    check(pitch == 1920, "the row is padded to the alignment GBM uses");
    check(carrierWidth * 4 == pitch, "the carrier's pixels cover the padded row");
    check(carrierHeight == 1620, "an odd number of chroma rows is rounded up");

    check(!uml::va::planarCarrier(VA_FOURCC_ARGB, 64, 64, carrierWidth, carrierHeight, pitch),
          "a packed format needs no carrier");
}

// A surface decoded as packed RGB still has to be readable as NV12, so the
// inverse conversion has to come back to where it started.
void testArgbToNv12()
{
    constexpr unsigned width = 16;
    constexpr unsigned height = 8;
    std::vector<std::uint8_t> luma(width * height);
    std::vector<std::uint8_t> chroma(width * height / 2);
    for (unsigned row = 0; row < height; ++row)
        for (unsigned column = 0; column < width; ++column)
            luma[row * width + column] = std::uint8_t(80 + 4 * column);
    // Colours a decoder really produces: chroma near the neutral point, so the
    // round trip measures the conversion rather than gamut clipping.
    for (unsigned row = 0; row < height / 2; ++row)
        for (unsigned column = 0; column < width / 2; ++column) {
            chroma[row * width + 2 * column] = std::uint8_t(120 + 2 * column);
            chroma[row * width + 2 * column + 1] = std::uint8_t(136 - 2 * column);
        }
    const uml::va::ConstPlaneView source[2] {{luma.data(), width}, {chroma.data(), width}};
    const uml::va::Region region {0, 0, width, height};

    std::vector<std::uint8_t> packed(width * height * 4);
    const uml::va::PlaneView argb {packed.data(), width * 4};
    uml::va::convertNv12ToArgb(source, region, argb, region, false);

    std::vector<std::uint8_t> backLuma(width * height, 0);
    std::vector<std::uint8_t> backChroma(width * height / 2, 0);
    const uml::va::PlaneView destination[2] {{backLuma.data(), width}, {backChroma.data(), width}};
    uml::va::convertArgbToNv12(uml::va::ConstPlaneView {packed.data(), width * 4}, region,
                               destination, region, false);

    int worstLuma = 0;
    for (std::size_t index = 0; index < luma.size(); ++index)
        worstLuma = std::max(worstLuma, std::abs(int(luma[index]) - int(backLuma[index])));
    int worstChroma = 0;
    for (std::size_t index = 0; index < chroma.size(); ++index)
        worstChroma = std::max(worstChroma, std::abs(int(chroma[index]) - int(backChroma[index])));
    check(worstLuma <= 2, "a round trip through packed RGB preserves luma");
    check(worstChroma <= 3, "a round trip through packed RGB preserves chroma");

    // The fast path a video-processing blit takes when the decoder already
    // produced the destination format: a plain copy, byte for byte.
    std::vector<std::uint8_t> blit(width * height * 4, 0);
    uml::va::copyPacked32(uml::va::ConstPlaneView {packed.data(), width * 4}, region,
                          uml::va::PlaneView {blit.data(), width * 4}, region);
    check(std::memcmp(blit.data(), packed.data(), blit.size()) == 0,
          "the packed fast path copies the picture unchanged");
}

// Rectangles come from the application: a negative corner, a size past the
// edge and a sum that would wrap 32 bits are refused, and the edges are in.
void testRegionBounds()
{
    using uml::va::regionWithin;
    const auto whole = regionWithin(0, 0, 64, 48, 64, 48);
    check(whole && whole->width == 64 && whole->height == 48, "an image is a region of itself");
    const auto corner = regionWithin(60, 40, 4, 8, 64, 48);
    check(corner && corner->x == 60 && corner->y == 40, "a region may touch the far edges");
    check(regionWithin(64, 48, 0, 0, 64, 48).has_value(), "an empty region at the far corner is inside");
    check(!regionWithin(-2, 0, 10, 10, 64, 48), "a negative x is refused");
    check(!regionWithin(0, -1, 10, 10, 64, 48), "a negative y is refused");
    check(!regionWithin(65, 0, 0, 0, 64, 48), "a corner past the edge is refused");
    check(!regionWithin(61, 0, 4, 1, 64, 48), "a region past the right edge is refused");
    check(!regionWithin(0, 45, 1, 4, 64, 48), "a region past the bottom edge is refused");
    check(!regionWithin(8, 0, 0xfffffffcu, 1, 64, 48), "a width that would wrap past the edge is refused");
    check(!regionWithin(0, 8, 1, 0xfffffffcu, 64, 48), "a height that would wrap past the edge is refused");
}

// An imported descriptor is the application's, so its layout is checked
// against the format before the driver maps and copies through it.
void testImportedLayout()
{
    constexpr unsigned width = 64;
    constexpr unsigned height = 48;
    constexpr std::size_t size = std::size_t(width) * height * 3 / 2;
    const int fd = memfd_create("va-v4l2-import", MFD_CLOEXEC);
    check(fd >= 0 && ftruncate(fd, off_t(size)) == 0, "an import fixture exists");
    if (fd < 0) return;
    const std::uint32_t offset[2] {0, width * height};
    auto adopted = [fd, &offset](unsigned planes, const std::uint32_t* pitch, std::size_t bytes) {
        std::string error;
        const int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        auto buffer = uml::va::PixelBuffer::adopt(duplicate, VA_FOURCC_NV12, width, height, planes, pitch, offset,
                                                  0, bytes, error);
        if (!buffer) ::close(duplicate);
        return buffer != nullptr;
    };
    const std::uint32_t pitch[2] {width, width};
    check(adopted(2, pitch, size), "a tightly packed NV12 descriptor is adopted");
    check(adopted(2, pitch, 0), "a descriptor without a size is adopted at its planes' extent");
    check(!adopted(2, pitch, size - 1), "a descriptor smaller than its planes is refused");
    check(!adopted(1, pitch, 0), "an NV12 descriptor without its chroma plane is refused");
    const std::uint32_t narrow[2] {width / 2, width};
    check(!adopted(2, narrow, 0), "rows shorter than the surface width are refused");

    // A descriptor imported again is reused only for the same image.
    std::string error;
    const int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    auto buffer = uml::va::PixelBuffer::adopt(duplicate, VA_FOURCC_NV12, width, height, 2, pitch, offset, 0,
                                              size, error);
    check(buffer != nullptr, "the reuse fixture is adopted");
    if (!buffer) ::close(duplicate);
    else {
        check(buffer->holds(VA_FOURCC_NV12, width, height), "the same image reuses the buffer");
        check(!buffer->holds(VA_FOURCC_NV12, width * 2, height), "a wider image does not reuse the buffer");
        check(!buffer->holds(VA_FOURCC_NV12, width, height * 2), "a taller image does not reuse the buffer");
        check(!buffer->holds(VA_FOURCC_I420, width, height), "another format does not reuse the buffer");
    }
    ::close(fd);
}

}

int main()
{
    testHeaders();
    testShowExistingByte();
    testSuperframes();
    testPixels();
    testLayout();
    testCapturedPlaneBounds();
    testMemoryStorage();
    testCapturePolicy();
    testCapturePool();
    testPlanarCarrier();
    testArgbToNv12();
    testRegionBounds();
    testImportedLayout();
    uml::va::test::runPoolTests();
    uml::va::test::runEncoderTests();
    uml::va::test::runDriverEncodeTests();
    uml::va::test::runAnnexbTests();
    uml::va::test::runGbmPlanarTests();
    if (const int failures = uml::va::test::failureCount()) {
        std::fprintf(stderr, "UML_VA_V4L2_TESTS: FAIL (%d)\n", failures);
        return 1;
    }
    std::puts("UML_VA_V4L2_TESTS: PASS");
    return 0;
}
