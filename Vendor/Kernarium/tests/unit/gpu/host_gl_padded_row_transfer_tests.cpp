// SPDX-License-Identifier: GPL-2.0-or-later

// A guest buffer whose rows are padded, such as the 342 pixel wide BGRA
// buffer Android's gralloc lays out in a 344 pixel pitch, is locked for
// reading and unlocked after writing with transfers. Sending it one row per
// transfer costs the renderer one readback or upload per row, and a tall
// buffer then takes hundreds of milliseconds to lock. The guest can instead
// send the whole box in one transfer that names the pitch as its stride, as a
// VIRGL_CCMD_TRANSFER3D in a command buffer or as a control-queue transfer.
//
// The contract under test: one transfer with the pitch as its stride, for the
// whole texture or for a rectangle of it taken from the offset of its first
// byte, lands every row at the pitch in both directions and leaves the padding
// of each guest row and the texels outside the rectangle as they were. A
// transfer without a stride takes the texture's width as the row length, which
// is why the stride has to be named.

#include "host_gl_stream_support.hpp"

#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace uml::gpu::testing::stream;
using uml::gpu::testing::skipWithoutRenderer;
using uml::gpu::testing::virglFormatB8g8r8a8Unorm;

// virgl_protocol.h values.
constexpr std::uint32_t commandClear = 7;
constexpr std::uint32_t commandTransfer3d = 43;
constexpr std::uint32_t transfer3dPayloadWords = 13;
constexpr std::uint32_t transferToHost = 1;
constexpr std::uint32_t transferFromHost = 2;
constexpr std::uint32_t clearColor0 = 4;

constexpr std::uint32_t drawContext = contextId;
constexpr std::uint32_t mapperContext = 2;
constexpr std::uint32_t resourceId = 11;
constexpr std::uint32_t width = 342;
constexpr std::uint32_t height = 604;
constexpr std::uint32_t pixelBytes = 4;
constexpr std::uint32_t pitch = 344 * pixelBytes;
constexpr std::uint8_t sentinel = 0xa5;

enum class Route { controlQueue, commandBuffer };

struct Rectangle {
    std::uint32_t x;
    std::uint32_t y;
    std::uint32_t width;
    std::uint32_t height;
};

constexpr Rectangle wholeTexture {0, 0, width, height};
constexpr Rectangle inner {5, 7, 101, 53};

// Whether the byte `byte` of the guest row `row` is one the rectangle covers.
bool covers(Rectangle rectangle, std::uint32_t row, std::uint32_t byte)
{
    return row >= rectangle.y && row < rectangle.y + rectangle.height &&
           byte >= rectangle.x * pixelBytes &&
           byte < (rectangle.x + rectangle.width) * pixelBytes;
}

const char* routeName(Route route)
{
    return route == Route::controlQueue ? "control queue" : "TRANSFER3D";
}

class Scene final {
public:
    explicit Scene(uml::gpu::VirglRenderer& renderer) : renderer_ {renderer}
    {
        require(renderer_.createContext(
                    drawContext, virgl2CapsetId, "draw") == 0,
            "the draw context could not be created");
        require(renderer_.createContext(
                    mapperContext, virgl2CapsetId, "mapper") == 0,
            "the mapper context could not be created");
        require(renderer_.createResource3d(uml::gpu::VirglRenderer::Resource3d {
                    .resourceId = resourceId,
                    .target = uml::gpu::testing::pipeTexture2d,
                    .format = virglFormatB8g8r8a8Unorm,
                    .bind = uml::gpu::testing::virglBindRenderTarget |
                            uml::gpu::testing::virglBindSamplerView,
                    .width = width,
                    .height = height,
                    .depth = 1,
                    .arraySize = 1,
                }) == 0,
            "the texture could not be created");
        storage_.assign(alignUp(pitch * height, 4096U), sentinel);
        vectors_[0] = {.iov_base = storage_.data(), .iov_len = storage_.size()};
        require(renderer_.attachBacking(resourceId, vectors_) == 0,
            "the guest rows could not be attached");
        require(renderer_.attachResource(drawContext, resourceId) == 0 &&
                    renderer_.attachResource(mapperContext, resourceId) == 0,
            "the texture could not be attached to both contexts");
        Commands commands;
        commands.add(commandCreateObject, objectSurface,
            {surface_, resourceId, virglFormatB8g8r8a8Unorm, 0, 0});
        require(commands.submit(renderer_) == 0,
            "the surface could not be made");
    }

    ~Scene()
    {
        (void)renderer_.detachResource(drawContext, resourceId);
        (void)renderer_.detachResource(mapperContext, resourceId);
        renderer_.detachBacking(resourceId);
        renderer_.unrefResource(resourceId);
        renderer_.destroyContext(mapperContext);
        renderer_.destroyContext(drawContext);
    }

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // The GPU paints every texel one grey and the guest waits for it. All four
    // channels are the value, so the bytes are the same whatever the order.
    void paint(std::uint8_t value)
    {
        const float level = static_cast<float>(value) / 255.0F;
        Commands commands;
        commands.add(commandSetFramebufferState, 0, {1, 0, surface_});
        commands.add(commandClear, 0,
            {clearColor0, bits(level), bits(level), bits(level), bits(level), 0,
                0, 0});
        require(commands.submit(renderer_) == 0, "the paint failed");
        wait(drawContext);
    }

    void fillGuestRows(std::uint32_t seed)
    {
        std::fill(storage_.begin(), storage_.end(), sentinel);
        for (std::uint32_t row = 0; row < height; ++row) {
            for (std::uint32_t byte = 0; byte < width * pixelBytes; ++byte)
                storage_[static_cast<std::size_t>(row) * pitch + byte] =
                    static_cast<std::uint8_t>((row * 7U + byte + seed) & 0xffU);
        }
        written_ = storage_;
    }

    void clearGuestRows()
    {
        std::fill(storage_.begin(), storage_.end(), sentinel);
    }

    // One transfer of the box `rectangle`, whose first byte is at
    // y * pitch + x * pixelBytes, with `stride` as its row length.
    void transfer(bool toHost, Route route, Rectangle rectangle,
        std::uint32_t stride)
    {
        const std::uint32_t offset =
            rectangle.y * pitch + rectangle.x * pixelBytes;
        if (route == Route::controlQueue) {
            const uml::gpu::VirglRenderer::Box box {
                .x = rectangle.x,
                .y = rectangle.y,
                .z = 0,
                .width = rectangle.width,
                .height = rectangle.height,
                .depth = 1};
            require(renderer_.transferResource(toHost, resourceId,
                        mapperContext, 0, stride, 0, box, offset) == 0,
                "the control-queue transfer failed");
        } else {
            const std::array<std::uint32_t, 14> words {
                (transfer3dPayloadWords << 16U) | commandTransfer3d,
                resourceId, 0, 0, stride, 0, rectangle.x, rectangle.y, 0,
                rectangle.width, rectangle.height, 1, offset,
                toHost ? transferToHost : transferFromHost};
            require(renderer_.submit(mapperContext,
                        {reinterpret_cast<const std::uint8_t*>(words.data()),
                            sizeof(words)}) == 0,
                "the TRANSFER3D command buffer failed");
        }
        wait(mapperContext);
    }

    // After a read: the rectangle's bytes hold `value`, every other byte of
    // the guest rows, padding included, still holds the sentinel.
    [[nodiscard]] bool rowsHold(std::uint8_t value, Rectangle rectangle) const
    {
        for (std::uint32_t row = 0; row < height; ++row) {
            for (std::uint32_t byte = 0; byte < pitch; ++byte) {
                const bool inside = covers(rectangle, row, byte);
                const std::uint8_t expected = inside ? value : sentinel;
                const std::uint8_t got =
                    storage_[static_cast<std::size_t>(row) * pitch + byte];
                if (got != expected) {
                    std::fprintf(stderr,
                        "  row %u byte %u: %02x, expected %02x\n", row, byte,
                        got, expected);
                    return false;
                }
            }
        }
        return true;
    }

    // After a write: the texture holds the guest bytes inside the rectangle
    // and `outside` everywhere else, and the guest rows were not touched.
    [[nodiscard]] bool textureHolds(std::uint8_t outside, Rectangle rectangle)
    {
        std::vector<std::uint8_t> texels(
            static_cast<std::size_t>(width) * height * pixelBytes);
        require(renderer_.readResourceRectangle(
                    resourceId, texels, 0, 0, width, height) == 0,
            "the texture could not be read");
        for (std::uint32_t row = 0; row < height; ++row) {
            for (std::uint32_t byte = 0; byte < width * pixelBytes; ++byte) {
                const bool inside = covers(rectangle, row, byte);
                const std::uint8_t expected = inside
                    ? written_[static_cast<std::size_t>(row) * pitch + byte]
                    : outside;
                const std::uint8_t got = texels
                    [static_cast<std::size_t>(row) * width * pixelBytes + byte];
                if (got != expected) {
                    std::fprintf(stderr,
                        "  texel row %u byte %u: %02x, expected %02x\n", row,
                        byte, got, expected);
                    return false;
                }
            }
        }
        return storage_ == written_;
    }

private:
    void wait(std::uint32_t context)
    {
        require(renderer_.createFenceAndWait(
                    context, 0, ++fence_, nullptr) == 0,
            "a fence did not complete");
    }

    uml::gpu::VirglRenderer& renderer_;
    std::uint64_t fence_ {};
    std::uint32_t surface_ = firstTransientHandle;
    std::vector<std::uint8_t> storage_;
    std::vector<std::uint8_t> written_;
    std::array<iovec, 1> vectors_ {};
};

bool ok = true;

void report(bool good, const char* direction, Route route, const char* what)
{
    std::fprintf(stderr, "%-9s %-13s %-11s %s\n", direction, routeName(route),
        what, good ? "ok" : "WRONG");
    ok = ok && good;
}

} // namespace

int main()
{
    std::unique_ptr<uml::gpu::VirglRenderer> renderer;
    try {
        renderer = std::make_unique<uml::gpu::VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }
    Scene scene(*renderer);

    for (const Route route : {Route::controlQueue, Route::commandBuffer}) {
        for (const auto& [rectangle, what] :
            {std::pair {wholeTexture, "whole"},
                std::pair {inner, "rectangle"}}) {
            scene.paint(0x77);
            scene.clearGuestRows();
            scene.transfer(false, route, rectangle, pitch);
            report(scene.rowsHold(0x77, rectangle), "FROM_HOST", route, what);

            scene.paint(0x33);
            scene.fillGuestRows(3);
            scene.transfer(true, route, rectangle, pitch);
            report(scene.textureHolds(0x33, rectangle), "TO_HOST", route, what);
        }
    }

    // Without the stride the rows are taken at the texture's width, so the
    // pitch the guest laid them out at cannot be recovered. The check above
    // would pass for any pitch if this did not fail.
    scene.paint(0x77);
    scene.clearGuestRows();
    scene.transfer(false, Route::controlQueue, wholeTexture, 0);
    const bool tight = !scene.rowsHold(0x77, wholeTexture);
    std::fprintf(stderr, "FROM_HOST control queue no stride  %s\n",
        tight ? "rows are tight, as expected" : "rows landed at the pitch");
    ok = ok && tight;

    std::fputs(ok ? "Padded row transfer: ok\n"
                  : "Padded row transfer: FAILED\n",
        stderr);
    return ok ? 0 : 1;
}
