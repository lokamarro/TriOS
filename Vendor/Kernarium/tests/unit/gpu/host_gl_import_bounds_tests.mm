// SPDX-License-Identifier: GPL-2.0-or-later

// A guest names the layout of every image it imports and every box it
// transfers: format, size, stride, offset. The renderer lays a texture over,
// or copies out of, storage it did not size itself - an exporter's
// shared-memory segment, a Metal heap, a Metal texture the exporter
// published, a whole texture level it read back - so each of those numbers
// has to fit what is really there, or the request is refused.
//
// Every refusal below was a host out-of-bounds access before it was checked:
// a texture over memory past a segment or a heap, read back to the guest; a
// Metal texture read at four times the texel size of the pages sized for it,
// written past them; and a compressed readback cut that read a row past its
// buffer and sent it to the guest. The accepted cases keep the legitimate
// imports and transfers working.

#include "gpu_metal_test_support.hpp"
#include "host_gl_test_support.hpp"
#include "protocol.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"
#include "virgl_renderer.hpp"

#import <Metal/Metal.h>

extern "C" {
#include <virglrenderer.h>

// The same-process registry through which a Venus context publishes the
// Metal heap behind a blob it exports, and the texture it laid over it.
// Internal to the renderer library.
std::uint64_t virgl_resource_native_publish(int fd, int type, void* handle);
bool virgl_resource_native_set_metal_texture(
    std::uint64_t id,
    void* texture,
    std::uint64_t offset,
    std::uint32_t width,
    std::uint32_t height);
void virgl_resource_native_release(std::uint64_t id);
}

#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using uml::gpu::protocol::virgl2CapsetId;
using uml::gpu::testing::pipeTexture2d;
using uml::gpu::testing::skipWithoutRenderer;
using uml::gpu::testing::virglBindSamplerView;
using uml::gpu::testing::virglFormatB8g8r8a8Unorm;
using uml::testing::require;
using uml::testing::ScratchBacking;

// virgl_protocol.h and virgl_hw.h values the guest sends.
constexpr std::uint32_t virglCommandSetType = 49;
constexpr std::uint32_t setTypeLength = 10;
constexpr std::uint32_t virglFormatDxt1Rgb = 105;
// virgl_resource.h
constexpr int nativeHandleMetalHeap = 1;

constexpr std::uint32_t contextId = 1;
constexpr std::uint32_t bytesPerPixel = 4;
constexpr std::uint32_t importWidth = 64;
constexpr std::uint32_t importStride = importWidth * bytesPerPixel;
constexpr std::uint64_t segmentSize = 64 * 1024;
// An unlinked scratch file stands in for the exporter's segment: its vnode is
// what the native registry keys on, and its pages are what a plain import maps.
constexpr std::string_view segmentName = "uml-import-bounds.XXXXXX";

[[nodiscard]] std::uint8_t patternByte(
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t channel) noexcept
{
    return static_cast<std::uint8_t>(x * 3U + y * 7U + channel * 61U + 1U);
}

// `rows` rows of the pattern, each importStride bytes.
[[nodiscard]] std::vector<std::uint8_t> patternImage(std::uint32_t rows)
{
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(importStride) * rows);
    for (std::uint32_t y = 0; y < rows; ++y)
        for (std::uint32_t x = 0; x < importWidth; ++x)
            for (std::uint32_t channel = 0; channel < bytesPerPixel; ++channel)
                pixels[static_cast<std::size_t>(y) * importStride +
                    x * bytesPerPixel + channel] = patternByte(x, y, channel);
    return pixels;
}

class Importer final {
public:
    explicit Importer(uml::gpu::VirglRenderer& renderer) noexcept
        : renderer_ {renderer}
    {
    }

    // Imports the segment as a fresh blob and types it the way a guest's
    // PIPE_RESOURCE_SET_TYPE does. Returns what the typing command answered.
    [[nodiscard]] int import(
        int segment,
        std::uint64_t size,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t stride,
        std::uint32_t offset)
    {
        resourceId_ = nextResourceId_++;
        const int descriptor = ::dup(segment);
        require(descriptor >= 0, "the segment could not be duplicated");
        virgl_renderer_resource_import_blob_args arguments {};
        arguments.res_handle = resourceId_;
        arguments.blob_mem = VIRGL_RENDERER_BLOB_MEM_HOST3D;
        arguments.fd_type = VIRGL_RENDERER_BLOB_FD_TYPE_SHM;
        arguments.fd = descriptor;
        arguments.size = size;
        require(virgl_renderer_resource_import_blob(&arguments) == 0,
            "the segment could not be imported as a blob");
        require(renderer_.attachResource(contextId, resourceId_) == 0,
            "the blob could not be attached to the guest context");

        std::array<std::uint32_t, setTypeLength + 1> command {
            (setTypeLength << 16U) | virglCommandSetType,
            resourceId_,
            virglFormatB8g8r8a8Unorm,
            virglBindSamplerView,
            width,
            height,
            0,
            0,
            0,
            stride,
            offset,
        };
        return renderer_.submit(
            contextId,
            {reinterpret_cast<const std::uint8_t*>(command.data()),
                sizeof(command)});
    }

    [[nodiscard]] std::uint32_t resourceId() const noexcept
    {
        return resourceId_;
    }

    void release()
    {
        (void)renderer_.detachResource(contextId, resourceId_);
        renderer_.unrefResource(resourceId_);
    }

private:
    uml::gpu::VirglRenderer& renderer_;
    std::uint32_t resourceId_ {};
    std::uint32_t nextResourceId_ {100};
};

// The renderer may store an imported image either way up, and a texture that
// stores it may name its channels the other way round; what it may not do is
// show anything but the exporter's pixels.
[[nodiscard]] bool showsPattern(
    const std::uint8_t* pixels,
    std::uint32_t height,
    bool swapRedBlue) noexcept
{
    bool upright = true;
    bool flipped = true;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < importWidth; ++x) {
            for (std::uint32_t channel = 0; channel < bytesPerPixel;
                 ++channel) {
                const std::uint32_t stored =
                    swapRedBlue && channel != 1 && channel != 3
                    ? 2 - channel
                    : channel;
                const auto actual =
                    pixels[static_cast<std::size_t>(y) * importStride +
                        x * bytesPerPixel + stored];
                upright = upright && actual == patternByte(x, y, channel);
                flipped = flipped &&
                    actual == patternByte(x, height - 1 - y, channel);
            }
        }
    }
    return upright || flipped;
}

void requireReadBackPattern(
    uml::gpu::VirglRenderer& renderer,
    std::uint32_t resourceId,
    std::uint32_t height,
    const char* description)
{
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(importStride) * height);
    require(renderer.readResourceRectangle(
                resourceId, pixels, 0, 0, importWidth, height) == 0,
        "an accepted import could not be read back");
    require(showsPattern(pixels.data(), height, false), description);
}

// A texture refreshed by a copy is refreshed when something reads it through
// the GPU, which is what naming its Metal texture to a presenter does.
void requirePresentedPattern(
    uml::gpu::VirglRenderer& renderer,
    id<MTLDevice> device,
    std::uint32_t resourceId,
    std::uint32_t height,
    const char* description)
{
    uml::gpu::VirglRenderer::NativeTexture native;
    require(renderer.resourceMetalTexture(resourceId, native) == 0 &&
            native.texture != nullptr,
        "an accepted import named no Metal texture");
    id<MTLTexture> texture = (__bridge id<MTLTexture>)native.texture;
    const bool bgra = texture.pixelFormat == MTLPixelFormatBGRA8Unorm;
    require(bgra || texture.pixelFormat == MTLPixelFormatRGBA8Unorm,
        "an imported texture does not store 8-bit channels");

    const auto copy = uml::gpu::testing::readTexture(
        [device newCommandQueue], texture, importWidth, height, importStride);
    require(showsPattern(copy.data(), height, !bgra), description);
}

// A shared-memory segment: every row the texture spans has to lie inside it.
void sharedMemoryImportsFitTheirSegment(
    uml::gpu::VirglRenderer& renderer,
    Importer& importer)
{
    const ScratchBacking segment {segmentName, segmentSize};
    const std::uint32_t rows =
        static_cast<std::uint32_t>(segmentSize / importStride);
    const auto contents = patternImage(rows);
    require(::pwrite(segment.descriptor(), contents.data(), contents.size(), 0) ==
            static_cast<ssize_t>(contents.size()),
        "the segment could not be filled");

    require(importer.import(segment.descriptor(), segmentSize, importWidth, rows,
                importStride, 0) == 0,
        "an image filling its segment exactly was refused");
    requireReadBackPattern(renderer, importer.resourceId(), rows,
        "an image filling its segment did not show the segment's pixels");
    importer.release();

    require(importer.import(segment.descriptor(), segmentSize, importWidth,
                rows / 2, importStride, segmentSize / 2) == 0,
        "an image in the second half of its segment was refused");
    importer.release();

    require(importer.import(segment.descriptor(), segmentSize, importWidth,
                rows * 8, importStride, 0) != 0,
        "an image eight times its segment was imported");
    importer.release();
    require(importer.import(segment.descriptor(), segmentSize, importWidth,
                rows / 2 + 1, importStride, segmentSize / 2) != 0,
        "an image running one row past its segment's end was imported");
    importer.release();
    require(importer.import(segment.descriptor(), segmentSize, importWidth, 1,
                importStride, segmentSize * 2) != 0,
        "an image starting past its segment's end was imported");
    importer.release();
    require(importer.import(segment.descriptor(), segmentSize, importWidth, 16,
                importStride / 2, 0) != 0,
        "a stride shorter than a row was accepted");
    importer.release();
    require(importer.import(segment.descriptor(), segmentSize, importWidth, 16,
                importStride + 2, 0) != 0,
        "a stride that is not a whole number of texels was accepted");
    importer.release();
}

// A blob a Venus context allocated from a Metal heap: its rows are the heap's
// bytes, and the heap is all there is.
void heapImportsFitTheirHeap(
    id<MTLDevice> device,
    Importer& importer)
{
    MTLHeapDescriptor* descriptor = [MTLHeapDescriptor new];
    descriptor.type = MTLHeapTypePlacement;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.size = segmentSize;
    id<MTLHeap> heap = [device newHeapWithDescriptor:descriptor];
    require(heap != nil, "the Metal heap could not be created");
    const std::uint64_t heapSize = heap.size;
    const std::uint32_t rows =
        static_cast<std::uint32_t>(heapSize / importStride);

    const ScratchBacking segment {segmentName, heapSize};
    const std::uint64_t native = virgl_resource_native_publish(
        segment.descriptor(), nativeHandleMetalHeap, (__bridge void*)heap);
    require(native != 0, "the Metal heap could not be published");

    require(importer.import(segment.descriptor(), heapSize, importWidth, rows,
                importStride, 0) == 0,
        "an image filling its Metal heap exactly was refused");
    importer.release();
    require(importer.import(segment.descriptor(), heapSize, importWidth, rows * 8,
                importStride, 0) != 0,
        "an image eight times its Metal heap was imported");
    importer.release();
    require(importer.import(segment.descriptor(), heapSize, importWidth, rows,
                importStride, importStride) != 0,
        "an image running one row past its Metal heap was imported");
    importer.release();

    virgl_resource_native_release(native);
}

// An optimal texture the exporter published: it is read into pages sized for
// the guest's format, at the texture's own texel size, so the two must agree.
void textureImportsMatchTheTexturesTexelSize(
    uml::gpu::VirglRenderer& renderer,
    id<MTLDevice> device,
    Importer& importer)
{
    constexpr std::uint32_t side = importWidth;
    for (const MTLPixelFormat format : {
             MTLPixelFormatRGBA32Float,
             MTLPixelFormatRGBA16Float,
             MTLPixelFormatR8Unorm,
             MTLPixelFormatBGRA8Unorm,
         }) {
        MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:format
                                         width:side
                                        height:side
                                     mipmapped:NO];
        textureDescriptor.storageMode = MTLStorageModeShared;
        textureDescriptor.usage = MTLTextureUsageShaderRead;
        const MTLSizeAndAlign placement =
            [device heapTextureSizeAndAlignWithDescriptor:textureDescriptor];
        MTLHeapDescriptor* heapDescriptor = [MTLHeapDescriptor new];
        heapDescriptor.type = MTLHeapTypePlacement;
        heapDescriptor.storageMode = MTLStorageModeShared;
        heapDescriptor.size = placement.size;
        id<MTLHeap> heap = [device newHeapWithDescriptor:heapDescriptor];
        require(heap != nil, "the texture's Metal heap could not be created");
        id<MTLTexture> texture =
            [heap newTextureWithDescriptor:textureDescriptor offset:0];
        require(texture != nil, "the heap texture could not be created");
        require(texture.buffer == nil, "the heap texture is not optimal");

        const bool matches = format == MTLPixelFormatBGRA8Unorm;
        if (matches) {
            const auto pixels = patternImage(side);
            [texture replaceRegion:MTLRegionMake2D(0, 0, side, side)
                       mipmapLevel:0
                         withBytes:pixels.data()
                       bytesPerRow:importStride];
        }

        const ScratchBacking segment {segmentName, heap.size};
        const std::uint64_t native = virgl_resource_native_publish(
            segment.descriptor(), nativeHandleMetalHeap, (__bridge void*)heap);
        require(native != 0, "the texture's Metal heap could not be published");
        require(virgl_resource_native_set_metal_texture(
                    native, (__bridge void*)texture, 0, side, side),
            "the heap texture could not be published");

        const int result = importer.import(
            segment.descriptor(), heap.size, side, side, importStride, 0);
        if (matches) {
            require(result == 0,
                "a texture of the guest's own texel size was refused");
            requirePresentedPattern(renderer, device, importer.resourceId(),
                side, "an imported texture did not show the texture's pixels");
        } else {
            require(result != 0,
                "a texture of another texel size was imported under a "
                "4-byte format");
        }
        importer.release();
        virgl_resource_native_release(native);
    }
}

// A compressed level is transferred in whole blocks, so a box has to start on
// a block and end on one or at the level's edge, as GL requires. The level
// readback cuts the box out of a buffer holding exactly the level.
void compressedTransfersStayOnBlocks(uml::gpu::VirglRenderer& renderer)
{
    constexpr std::uint32_t blockBytes = 8;
    struct Level {
        std::uint32_t resourceId;
        std::uint32_t side;
    };
    for (const Level level : {Level {60, 8}, Level {61, 6}}) {
        require(renderer.createResource3d(uml::gpu::VirglRenderer::Resource3d {
                    .resourceId = level.resourceId,
                    .target = pipeTexture2d,
                    .format = virglFormatDxt1Rgb,
                    .bind = virglBindSamplerView,
                    .width = level.side,
                    .height = level.side,
                    .depth = 1,
                    .arraySize = 1,
                }) == 0,
            "a BC1 texture could not be created");
        const std::uint32_t blocks = (level.side + 3) / 4;
        std::vector<std::uint8_t> backing(
            static_cast<std::size_t>(blocks) * blocks * blockBytes);
        for (std::size_t index = 0; index < backing.size(); ++index)
            backing[index] = static_cast<std::uint8_t>(index * 5U + 3U);
        const std::vector<std::uint8_t> stored = backing;
        std::array<iovec, 1> vectors {
            iovec {.iov_base = backing.data(), .iov_len = backing.size()},
        };
        require(renderer.attachBacking(level.resourceId, vectors) == 0,
            "the BC1 texture's backing could not be attached");
        require(renderer.attachResource(contextId, level.resourceId) == 0,
            "the BC1 texture could not be attached to the guest context");

        const auto transfer = [&](bool toHost,
                                  std::uint32_t x,
                                  std::uint32_t y,
                                  std::uint32_t width,
                                  std::uint32_t height) {
            return renderer.transferResource(toHost, level.resourceId,
                contextId, 0, 0, 0,
                uml::gpu::VirglRenderer::Box {
                    .x = x,
                    .y = y,
                    .width = width,
                    .height = height,
                    .depth = 1,
                },
                0);
        };
        require(transfer(true, 0, 0, level.side, level.side) == 0,
            "a whole BC1 level could not be written");

        if (level.side == 8) {
            require(transfer(false, 1, 1, 7, 7) != 0,
                "a BC1 box starting inside a block was read back");
            require(transfer(false, 0, 0, 7, 8) != 0,
                "a BC1 box ending inside a block short of the edge was "
                "read back");
            require(transfer(true, 1, 1, 7, 7) != 0,
                "a BC1 box starting inside a block was written");

            std::fill(backing.begin(), backing.end(), std::uint8_t {});
            require(transfer(false, 4, 4, 4, 4) == 0,
                "a block-aligned BC1 box could not be read back");
            require(std::equal(backing.begin(), backing.begin() + blockBytes,
                        stored.begin() + 3 * blockBytes),
                "a block-aligned BC1 box read back the wrong block");
        } else {
            // A level whose side is not a whole number of blocks ends on its
            // edge, not on a block boundary.
            require(transfer(false, 0, 0, 5, 6) != 0,
                "a BC1 box ending inside a block short of the edge was "
                "read back");
            std::fill(backing.begin(), backing.end(), std::uint8_t {});
            require(transfer(false, 0, 0, 6, 6) == 0,
                "a BC1 box reaching the level's edge was refused");
            require(backing == stored,
                "a whole BC1 level read back other bytes than were written");
        }

        require(renderer.detachResource(contextId, level.resourceId) == 0,
            "the BC1 texture could not be detached");
        renderer.detachBacking(level.resourceId);
        renderer.unrefResource(level.resourceId);
    }
}

} // namespace

// With an argument, runs only the named group: shm, heap, texture or
// compressed.
int main(int argc, char** argv)
{
    const std::string only = argc > 1 ? argv[1] : "";
    const auto runs = [&only](const char* group) {
        return only.empty() || only == group;
    };
    @autoreleasepool {
        std::unique_ptr<uml::gpu::VirglRenderer> renderer;
        try {
            renderer = std::make_unique<uml::gpu::VirglRenderer>();
        } catch (const std::exception& error) {
            return skipWithoutRenderer(error);
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "no Metal device");

        require(renderer->createContext(contextId, virgl2CapsetId, "importer")
                == 0,
            "the guest context could not be created");
        Importer importer {*renderer};
        if (runs("shm"))
            sharedMemoryImportsFitTheirSegment(*renderer, importer);
        if (runs("heap"))
            heapImportsFitTheirHeap(device, importer);
        if (runs("texture"))
            textureImportsMatchTheTexturesTexelSize(*renderer, device, importer);
        if (runs("compressed"))
            compressedTransfersStayOnBlocks(*renderer);
        renderer->destroyContext(contextId);
    }
    std::fputs("Import bounds: ok\n", stderr);
    return 0;
}
