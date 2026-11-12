// SPDX-License-Identifier: GPL-2.0-or-later

// A guest that is not offered a compressed format it has to expose decodes the
// application's texture and stores it uncompressed. OpenGL ES 3.0 requires
// ETC2 and EAC, and Mesa exposes ETC1 and ASTC by decoding them too, so on a
// renderer that offered none of them every such texture cost RGBA8: eight
// times an ETC2 RGB texture, sixteen times an ASTC 8x8 one.
//
// The contract under test: the renderer offers ETC1, ETC2, EAC and ASTC in
// linear and sRGB form, an image of each samples as the format decodes it, a
// copy moves its blocks as OpenGL ES defines, and the host stores the image at
// its compressed size, not decoded.

#include "host_gl_stream_support.hpp"
#include "host_gl_test_support.hpp"
#include "host_gpu_memory.hpp"

#include <virgl_hw.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace {

using namespace uml::gpu::testing::stream;
using uml::gpu::testing::skipWithoutRenderer;

int failureCount = 0;

void check(bool condition, const char* description)
{
    if (condition)
        return;
    std::fprintf(stderr, "Compressed formats: %s\n", description);
    ++failureCount;
}

// caps_v1 is { uint32_t max_version; format_mask sampler; ... }, and each mask
// is 16 uint32_t words indexed by format identifier.
constexpr std::size_t samplerMaskOffset = 4;
constexpr std::size_t maskWords = 16;

[[nodiscard]] bool advertised(std::span<const std::uint8_t> capset,
    std::uint32_t format)
{
    const std::size_t word = format / 32U;
    if (word >= maskWords)
        return false;
    const std::size_t offset = samplerMaskOffset + word * sizeof(std::uint32_t);
    if (offset + sizeof(std::uint32_t) > capset.size())
        return false;
    std::uint32_t value = 0;
    for (std::size_t byte = 0; byte < sizeof(value); ++byte)
        value |= static_cast<std::uint32_t>(capset[offset + byte]) << (8U * byte);
    return ((value >> (format % 32U)) & 1U) != 0U;
}

// One block of each layout, repeated over the image, with the texel every
// pixel of it decodes to.
//
// ETC1 and ETC2 individual mode: both halves have the 4-bit base colour
// (8, 4, 2), widened to (136, 68, 34), codeword table 0 and every pixel index
// zero, which adds 2: (138, 70, 36).
constexpr std::array<std::uint8_t, 8> etcColour {
    0x88, 0x44, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00};
// EAC alpha with base 128 and multiplier 0: every pixel is 128.
constexpr std::array<std::uint8_t, 8> eacAlpha {
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
// EAC R11 with base 128, multiplier 0 (an eighth), table 0 and index zero:
// 128 * 8 + 4 - 3 = 1025 of 2047, which an 8-bit target holds as 128.
constexpr std::array<std::uint8_t, 8> eacRed {
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
// An ASTC void-extent LDR block of the UNORM16 colour (0x8888, 0x4444,
// 0x2222, 0xffff): (136, 68, 34, 255) linear, and in sRGB the linear values
// (63, 14, 4, 255) those code values stand for.
constexpr std::array<std::uint8_t, 16> astcConstant {
    0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x88, 0x88, 0x44, 0x44, 0x22, 0x22, 0xff, 0xff};

struct Format {
    std::uint32_t virglFormat;
    const char* name;
    std::uint32_t blockWidth;
    std::uint32_t blockHeight;
    std::uint32_t blockBytes;
    std::array<std::uint8_t, 4> texel;
};

constexpr std::array formats {
    Format {VIRGL_FORMAT_ETC1_RGB8, "ETC1_RGB8", 4, 4, 8, {138, 70, 36, 255}},
    Format {VIRGL_FORMAT_ETC2_RGB8, "ETC2_RGB8", 4, 4, 8, {138, 70, 36, 255}},
    Format {VIRGL_FORMAT_ETC2_RGBA8, "ETC2_RGBA8", 4, 4, 16, {138, 70, 36, 128}},
    Format {VIRGL_FORMAT_ETC2_R11_UNORM, "ETC2_R11_UNORM", 4, 4, 8, {128, 0, 0, 255}},
    Format {VIRGL_FORMAT_ASTC_4x4, "ASTC_4x4", 4, 4, 16, {136, 68, 34, 255}},
    Format {VIRGL_FORMAT_ASTC_8x8, "ASTC_8x8", 8, 8, 16, {136, 68, 34, 255}},
    Format {VIRGL_FORMAT_ASTC_4x4_SRGB, "ASTC_4x4_SRGB", 4, 4, 16, {63, 14, 4, 255}},
    Format {VIRGL_FORMAT_ASTC_8x8_SRGB, "ASTC_8x8_SRGB", 8, 8, 16, {63, 14, 4, 255}},
};

[[nodiscard]] std::span<const std::uint8_t> block(const Format& format,
    std::array<std::uint8_t, 16>& scratch)
{
    switch (format.virglFormat) {
    case VIRGL_FORMAT_ETC1_RGB8:
    case VIRGL_FORMAT_ETC2_RGB8:
        return etcColour;
    case VIRGL_FORMAT_ETC2_RGBA8:
        std::memcpy(scratch.data(), eacAlpha.data(), eacAlpha.size());
        std::memcpy(scratch.data() + 8, etcColour.data(), etcColour.size());
        return scratch;
    case VIRGL_FORMAT_ETC2_R11_UNORM:
        return eacRed;
    default:
        return astcConstant;
    }
}

// An image of `blocks` by `blocks` copies of the format's block, written to the
// host as a guest would.
void writeBlocks(Image& image, const Format& format, std::uint32_t blocks)
{
    std::array<std::uint8_t, 16> scratch {};
    const auto one = block(format, scratch);
    for (std::size_t offset = 0; offset < image.bytes().size();
         offset += one.size())
        std::memcpy(image.bytes().data() + offset, one.data(), one.size());
    // A row of blocks is the stride, and a transfer box is in texels.
    const std::uint32_t stride = blocks * format.blockBytes;
    image.writeBox(stride, 0, 0, blocks * format.blockWidth,
        blocks * format.blockHeight);
}

void checkDecode(VirglRenderer& renderer, Pipeline& pipeline,
    const Format& format, std::uint32_t resourceId)
{
    constexpr std::uint32_t blocks = 2;
    const std::uint32_t width = blocks * format.blockWidth;
    const std::uint32_t height = blocks * format.blockHeight;
    Image image(renderer, resourceId, pipeTexture2d, format.virglFormat,
        virglBindSamplerView, width, height, 0,
        static_cast<std::size_t>(blocks) * blocks * format.blockBytes);
    writeBlocks(image, format, blocks);
    const auto view = pipeline.createView(resourceId, format.virglFormat, 0);
    const auto texels = pipeline.sample(view, width, height);
    unsigned wrong = 0;
    for (std::size_t texel = 0; texel < texels.size() / 4U; ++texel) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const int got = texels[texel * 4U + channel];
            const int wanted = format.texel[channel];
            if (got > wanted + 1 || got < wanted - 1) {
                if (wrong++ < 2) {
                    std::fprintf(stderr,
                        "%s texel %zu: got %u,%u,%u,%u wanted %u,%u,%u,%u\n",
                        format.name, texel, texels[texel * 4U],
                        texels[texel * 4U + 1], texels[texel * 4U + 2],
                        texels[texel * 4U + 3], format.texel[0],
                        format.texel[1], format.texel[2], format.texel[3]);
                }
                break;
            }
        }
    }
    check(wrong == 0, "an image did not sample as its format decodes");
}

// VIRGL_CCMD_RESOURCE_COPY_REGION and the formats a copy pairs with ASTC.
constexpr std::uint32_t commandResourceCopyRegion = 17;
constexpr std::uint32_t virglFormatR32g32b32a32Uint = 196;
constexpr std::uint32_t virglFormatAstc4x4 = VIRGL_FORMAT_ASTC_4x4;
constexpr std::uint32_t virglFormatAstc4x4Srgb = VIRGL_FORMAT_ASTC_4x4_SRGB;

void copyRegion(VirglRenderer& renderer, std::uint32_t source,
    std::uint32_t destination, std::uint32_t width, std::uint32_t height,
    std::uint64_t& fence)
{
    Commands commands;
    commands.add(commandResourceCopyRegion, 0,
        {destination, 0, 0, 0, 0, source, 0, 0, 0, 0, width, height, 1});
    require(commands.submit(renderer) == 0, "the copy was refused");
    require(renderer.createFenceAndWait(contextId, 0, ++fence, nullptr) == 0,
        "the copy did not finish");
}

[[nodiscard]] bool samplesAs(Pipeline& pipeline, std::uint32_t resourceId,
    std::uint32_t format, std::uint32_t width, std::uint32_t height,
    const std::array<std::uint8_t, 4>& wanted)
{
    const auto view = pipeline.createView(resourceId, format, 0);
    const auto texels = pipeline.sample(view, width, height);
    for (std::size_t texel = 0; texel < texels.size() / 4U; ++texel) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const int got = texels[texel * 4U + channel];
            if (got > wanted[channel] + 1 || got < wanted[channel] - 1) {
                std::fprintf(stderr, "copy texel %zu: got %u,%u,%u,%u\n", texel,
                    texels[texel * 4U], texels[texel * 4U + 1],
                    texels[texel * 4U + 2], texels[texel * 4U + 3]);
                return false;
            }
        }
    }
    return true;
}

// OpenGL ES copies raw blocks between the formats of one class, an ASTC
// image and its sRGB form, and between a compressed format and the
// uncompressed one whose texel is its block. A desktop host must do the same
// for the formats it offers, or the guest's copies leave the destination as
// it was.
void checkCopies(VirglRenderer& renderer, Pipeline& pipeline,
    std::uint32_t& resourceId, std::uint64_t& fence)
{
    constexpr std::uint32_t blocks = 2;
    constexpr std::uint32_t side = blocks * 4;
    constexpr std::size_t bytes = blocks * blocks * astcConstant.size();
    const Format& linear = formats[4];
    const Format& srgb = formats[6];
    require(linear.virglFormat == virglFormatAstc4x4 &&
            srgb.virglFormat == virglFormatAstc4x4Srgb,
        "the ASTC 4x4 rows moved");

    const std::uint32_t sourceId = resourceId++;
    Image source(renderer, sourceId, pipeTexture2d, virglFormatAstc4x4,
        virglBindSamplerView, side, side, 0, bytes);
    writeBlocks(source, linear, blocks);

    // The same blocks under the sRGB format decode to its colour.
    const std::uint32_t srgbId = resourceId++;
    Image sameClass(renderer, srgbId, pipeTexture2d, virglFormatAstc4x4Srgb,
        virglBindSamplerView, side, side, 0, bytes);
    copyRegion(renderer, sourceId, srgbId, side, side, fence);
    check(samplesAs(pipeline, srgbId, virglFormatAstc4x4Srgb, side, side,
              srgb.texel),
        "a copy from ASTC to its sRGB form did not move the blocks");

    // A block is one RGBA32UI texel: out as bits, and back in as blocks.
    const std::uint32_t rawId = resourceId++;
    Image raw(renderer, rawId, pipeTexture2d, virglFormatR32g32b32a32Uint,
        virglBindSamplerView, blocks, blocks, 0, bytes);
    copyRegion(renderer, sourceId, rawId, side, side, fence);
    require(raw.readBox(blocks * 16U, 0, 0, blocks, blocks) == 0,
        "the uncompressed copy could not be read");
    bool same = true;
    for (std::size_t offset = 0; offset < bytes; offset += astcConstant.size())
        same = same && std::memcmp(raw.bytes().data() + offset,
                           astcConstant.data(), astcConstant.size()) == 0;
    check(same, "a copy from ASTC to RGBA32UI did not move the block bits");

    const std::uint32_t backId = resourceId++;
    Image back(renderer, backId, pipeTexture2d, virglFormatAstc4x4Srgb,
        virglBindSamplerView, side, side, 0, bytes);
    copyRegion(renderer, rawId, backId, blocks, blocks, fence);
    check(samplesAs(pipeline, backId, virglFormatAstc4x4Srgb, side, side,
              srgb.texel),
        "a copy from RGBA32UI to ASTC did not make the blocks");
}

[[nodiscard]] std::uint64_t allocatedAfter(VirglRenderer& renderer,
    std::uint64_t& fence)
{
    require(renderer.createFenceAndWait(contextId, 0, ++fence, nullptr) == 0,
        "the renderer did not finish");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return uml::platform::hostGpuAllocatedBytes();
}

// A large image of the format costs the host its compressed size: the bytes
// Metal holds grow by about that much, not by the four bytes a texel of a
// decoded copy would take.
void checkStorage(VirglRenderer& renderer, const Format& format,
    std::uint32_t resourceId, std::uint64_t& fence)
{
    constexpr std::uint32_t side = 4096;
    const std::uint64_t compressed = static_cast<std::uint64_t>(side) * side /
        (format.blockWidth * format.blockHeight) * format.blockBytes;
    const std::uint64_t decoded = static_cast<std::uint64_t>(side) * side * 4U;
    const std::uint64_t before = allocatedAfter(renderer, fence);
    {
        Image image(renderer, resourceId, pipeTexture2d, format.virglFormat,
            virglBindSamplerView, side, side, 0,
            static_cast<std::size_t>(compressed));
        const std::uint64_t after = allocatedAfter(renderer, fence);
        const std::uint64_t grown = after > before ? after - before : 0;
        std::fprintf(stderr,
            "%s %ux%u: host grew %.2f MiB, compressed %.2f MiB, decoded %.2f "
            "MiB\n",
            format.name, side, side, grown / 1048576.0, compressed / 1048576.0,
            decoded / 1048576.0);
        check(grown >= compressed / 2,
            "the host did not show the image's storage at all");
        check(grown <= compressed * 2,
            "the host stored a compressed image decoded");
    }
}

} // namespace

int main()
{
    // Freed storage goes back to Metal at once, so that one image's storage is
    // not handed to the next and every image shows in the counter.
    ::setenv("ZINK_BO_CACHE_MAX_MIB", "0", 1);
    std::unique_ptr<VirglRenderer> renderer;
    try {
        renderer = std::make_unique<VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }

    const auto capset = renderer->capset(virgl2CapsetId).data;
    for (const Format& format : formats) {
        const bool offered = advertised(capset, format.virglFormat);
        std::fprintf(stderr, "virgl %s: advertised=%d\n", format.name,
            offered ? 1 : 0);
        check(offered, "a compressed format the host samples is not offered");
    }

    require(renderer->createContext(contextId, virgl2CapsetId, "compressed") ==
            0,
        "the guest context could not be created");
    Pipeline pipeline(*renderer);
    std::uint32_t resourceId = 10;
    for (const Format& format : formats) {
        if (advertised(capset, format.virglFormat))
            checkDecode(*renderer, pipeline, format, resourceId++);
    }
    std::uint64_t fence = 1000;
    if (advertised(capset, virglFormatAstc4x4) &&
        advertised(capset, virglFormatAstc4x4Srgb))
        checkCopies(*renderer, pipeline, resourceId, fence);
    for (const Format& format : formats) {
        if (advertised(capset, format.virglFormat))
            checkStorage(*renderer, format, resourceId++, fence);
    }

    if (failureCount != 0)
        return 1;
    std::fputs("Compressed formats: ok\n", stderr);
    return 0;
}
