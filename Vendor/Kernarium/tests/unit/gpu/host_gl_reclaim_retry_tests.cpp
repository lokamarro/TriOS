// SPDX-License-Identifier: GPL-2.0-or-later

// A guest context that draws and goes quiet keeps what its last batches used
// until it submits again: the textures its guest deleted stay allocated in the
// host driver, charged to the device memory pool, and no refusal on another
// thread can reach them. When a second guest context then asks for a texture
// that fits the budget except for what the first still holds, the pool refuses
// a request that the guest never overcommitted, and the guest sees an out of
// memory error that browsers answer by losing every WebGL context.
//
// The renderer therefore reclaims before it fails a creation: it finishes and
// trims every context it has, empties the deferred frees and the driver's
// caches, and creates the resource once more. The contract under test:
//
//  - the creations that fit the budget once the guest's deleted textures are
//    gone succeed, round after round, although another context holds those
//    textures' memory when each round starts;
//  - a request that does not fit even then is still refused with ENOMEM, in
//    bounded time (repeated refusals do not each pay for a full reclaim), and
//    the textures the guest holds keep their pixels; and
//  - once the guest releases them, the room is whole again.

#include "host_gl_stream_support.hpp"
#include "host_gpu_memory.hpp"

#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <vector>

namespace {

using namespace uml::gpu::testing::stream;
using uml::gpu::testing::skipWithoutRenderer;

constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
constexpr std::uint32_t textureSize = 1024;
constexpr std::uint32_t bytesPerPixel = 4;
constexpr std::uint64_t textureBytes =
    static_cast<std::uint64_t>(textureSize) * textureSize * bytesPerPixel;
// The producer context paints enough textures to take most of the room, and a
// round asks for as many again through the renderer's own context.
constexpr std::uint64_t roomBytes = 192 * mebibyte;
constexpr std::uint32_t producerTextures = 36;
constexpr std::uint32_t consumerTextures = 28;
constexpr std::uint32_t rounds = 3;
// Refusals past a full budget, and what a refusal may cost on average.
constexpr std::uint32_t overcommitAttempts = 40;
constexpr double refusalSecondsAllowed = 1.5;
// What the budget does not bound: the driver's own small allocations.
constexpr std::uint64_t slackBytes = 8 * mebibyte;

constexpr std::uint32_t commandClear = 7;
constexpr std::uint32_t commandDestroyObject = 3;
constexpr std::uint32_t clearColor0 = 4;
constexpr std::uint32_t firstProducerResource = 1000;
constexpr std::uint32_t firstConsumerResource = 1;

[[nodiscard]] bool isOutOfMemory(int result) noexcept
{
    return result == ENOMEM || result == -ENOMEM;
}

// What the process holds by the larger of Metal's allocation and the graphics
// footprint, since nothing here is imported; the pool counts at least this.
[[nodiscard]] std::uint64_t used() noexcept
{
    return std::max(uml::platform::hostGpuAllocatedBytes(),
        uml::platform::hostGraphicsFootprintBytes());
}

[[nodiscard]] double mib(std::uint64_t bytes) noexcept
{
    return static_cast<double>(bytes) / mebibyte;
}

// A texture made on the renderer's own context with guest storage, written and
// read back by transfer.
class Texture final {
public:
    Texture(uml::gpu::VirglRenderer& renderer, std::uint32_t resourceId)
        : renderer_ {renderer}
        , resourceId_ {resourceId}
        , storage_(static_cast<std::size_t>(textureBytes))
    {
        result_ = renderer_.createResource3d(
            uml::gpu::VirglRenderer::Resource3d {
                .resourceId = resourceId_,
                .target = uml::gpu::testing::pipeTexture2d,
                .format = uml::gpu::testing::virglFormatR8g8b8a8Unorm,
                .bind = uml::gpu::testing::virglBindRenderTarget |
                        uml::gpu::testing::virglBindSamplerView,
                .width = textureSize,
                .height = textureSize,
                .depth = 1,
                .arraySize = 1,
                .lastLevel = 0,
            });
        created_ = result_ == 0;
        if (!created_)
            return;
        // The renderer keeps this array, not a copy of it.
        vectors_[0] = {.iov_base = storage_.data(), .iov_len = storage_.size()};
        require(renderer_.attachBacking(resourceId_, vectors_) == 0,
            "the texture's guest storage could not be attached");
    }

    ~Texture()
    {
        if (!created_)
            return;
        renderer_.detachBacking(resourceId_);
        renderer_.unrefResource(resourceId_);
    }

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // What the renderer answered the creation with: 0, or the reason.
    [[nodiscard]] int result() const noexcept { return result_; }

    // Writes a pattern of its own and reads it back: true when the host gave
    // the texture storage that holds it.
    [[nodiscard]] bool stores(std::uint8_t pattern)
    {
        if (!created_)
            return false;
        std::fill(storage_.begin(), storage_.end(), pattern);
        if (transfer(true) != 0)
            return false;
        return readsAs(pattern);
    }

    [[nodiscard]] bool readsAs(std::uint8_t pattern)
    {
        if (!created_)
            return false;
        std::fill(storage_.begin(), storage_.end(),
            static_cast<std::uint8_t>(~pattern));
        if (transfer(false) != 0)
            return false;
        return std::all_of(storage_.begin(), storage_.end(),
            [pattern](std::uint8_t byte) { return byte == pattern; });
    }

private:
    [[nodiscard]] int transfer(bool toHost)
    {
        const uml::gpu::VirglRenderer::Box box {
            .width = textureSize,
            .height = textureSize,
            .depth = 1,
        };
        return renderer_.transferResource(toHost, resourceId_, 0, 0,
            textureSize * bytesPerPixel, 0, box, 0);
    }

    uml::gpu::VirglRenderer& renderer_;
    std::uint32_t resourceId_;
    int result_ {};
    bool created_ {};
    std::vector<std::uint8_t> storage_;
    std::array<iovec, 1> vectors_ {};
};

// The producer: a guest context that paints textures and deletes them, and
// then does nothing more.
class Producer final {
public:
    explicit Producer(uml::gpu::VirglRenderer& renderer)
        : renderer_ {renderer}
    {
        require(renderer_.createContext(
                    contextId, virgl2CapsetId, "producer") == 0,
            "the producer context could not be created");
    }

    ~Producer() { renderer_.destroyContext(contextId); }

    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;

    // Makes `count` textures and paints each, then waits for the GPU. The
    // textures stay with the guest until release.
    void paint(std::uint32_t count)
    {
        for (std::uint32_t index = 0; index < count; ++index) {
            const std::uint32_t resourceId = firstProducerResource + index;
            require(Image::create(renderer_, resourceId, pipeTexture2d,
                        virglFormatR8g8b8a8Unorm,
                        virglBindRenderTarget | virglBindSamplerView,
                        textureSize, textureSize, 0) == 0,
                "the producer could not make a texture: the room is too "
                "small for the test");
            require(renderer_.attachResource(contextId, resourceId) == 0,
                "the texture could not be attached to the producer");
            const std::uint32_t surface = nextSurface_++;
            const float level = static_cast<float>(index + 1) / 255.0F;
            Commands commands;
            commands.add(commandCreateObject, objectSurface,
                {surface, resourceId, virglFormatR8g8b8a8Unorm, 0, 0});
            commands.add(commandSetFramebufferState, 0, {1, 0, surface});
            commands.add(commandClear, 0,
                {clearColor0, bits(level), bits(level), bits(level),
                    bits(level), 0, 0, 0});
            // The guest unbinds the target before it destroys the surface.
            commands.add(commandSetFramebufferState, 0, {0, 0});
            commands.add(commandDestroyObject, objectSurface, {surface});
            require(commands.submit(renderer_) == 0, "the paint failed");
        }
        wait();
        painted_ = count;
    }

    // The guest deletes every texture it painted; the context goes quiet.
    void release()
    {
        for (std::uint32_t index = 0; index < painted_; ++index) {
            const std::uint32_t resourceId = firstProducerResource + index;
            (void)renderer_.detachResource(contextId, resourceId);
            renderer_.unrefResource(resourceId);
        }
        painted_ = 0;
    }

private:
    void wait()
    {
        require(renderer_.createFenceAndWait(
                    contextId, 0, ++fence_, nullptr) == 0,
            "the producer's work did not complete");
    }

    uml::gpu::VirglRenderer& renderer_;
    std::uint64_t fence_ {};
    std::uint32_t nextSurface_ = firstTransientHandle;
    std::uint32_t painted_ {};
};

} // namespace

int main()
{
    // The pool takes its limit when the renderer starts, and counts everything
    // the process holds, so the limit is what is held now plus the room, and
    // the reserve the pool keeps inside its limit.
    const std::uint64_t before = used();
    const std::uint64_t limit = before + roomBytes +
        uml::gpu::VirglRenderer::deviceMemoryReserveBytes;
    uml::gpu::VirglRenderer::setDeviceMemoryLimit(limit);
    std::unique_ptr<uml::gpu::VirglRenderer> renderer;
    try {
        renderer = std::make_unique<uml::gpu::VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }

    Producer producer(*renderer);
    std::uint32_t nextResourceId = firstConsumerResource;
    std::uint64_t peak = 0;

    // Rounds of a near miss: the producer's deleted textures still hold the
    // room when the consumer asks for it.
    std::uint32_t refused = 0;
    for (std::uint32_t round = 0; round < rounds; ++round) {
        producer.paint(producerTextures);
        const std::uint64_t painted = used();
        producer.release();
        const std::uint64_t held = used();
        std::vector<std::unique_ptr<Texture>> consumer;
        for (std::uint32_t index = 0; index < consumerTextures; ++index) {
            consumer.push_back(
                std::make_unique<Texture>(*renderer, nextResourceId++));
            if (consumer.back()->result() != 0) {
                require(isOutOfMemory(consumer.back()->result()),
                    "a texture was refused for a reason other than memory");
                ++refused;
            }
            peak = std::max(peak, used());
        }
        // The first texture, one in the middle and the last, which the retry
        // made, hold what the guest writes to them.
        std::uint32_t stored = 0;
        for (const std::uint32_t index :
            {0U, consumerTextures / 2, consumerTextures - 1}) {
            if (consumer[index]->stores(static_cast<std::uint8_t>(index + 1)))
                ++stored;
        }
        std::fprintf(stderr,
            "round %u: the producer's %u textures took %.1f MiB and %.1f MiB "
            "stayed after the guest deleted them; %u of %u textures of %.0f "
            "MiB were refused, %u of 3 checked hold their pixels (limit %.1f "
            "MiB over %.1f MiB at the start)\n",
            round, producerTextures,
            mib(painted > before ? painted - before : 0),
            mib(held > before ? held - before : 0), refused, consumerTextures,
            mib(textureBytes), stored, mib(limit - before), mib(before));
        require(stored == 3,
            "a texture the retry made has no storage for what the guest "
            "writes");
    }
    require(refused == 0, "a creation was refused although the room fit it");
    require(peak <= limit + slackBytes,
        "the host GPU held more than its budget");

    // A real overcommit: the guest holds the room, and what it asks for beyond
    // that is refused, again and again, without each refusal paying for a
    // full reclaim, and without losing what it holds.
    std::vector<std::unique_ptr<Texture>> held;
    std::vector<std::uint8_t> patterns;
    for (std::uint32_t index = 0; index < consumerTextures * 3; ++index) {
        held.push_back(
            std::make_unique<Texture>(*renderer, nextResourceId++));
        if (held.back()->result() != 0) {
            require(isOutOfMemory(held.back()->result()),
                "a texture was refused for a reason other than memory");
            held.pop_back();
            break;
        }
        const auto pattern = static_cast<std::uint8_t>(index + 101);
        // A write at a nearly full budget may be refused too, since it needs
        // a staging buffer: the texture only counts as written when it was
        // not.
        patterns.push_back(held.back()->stores(pattern) ? pattern : 0);
    }
    require(held.size() > 4, "the budget left no room for a few textures");
    require(held.size() < consumerTextures * 3,
        "the budget did not refuse a texture past its limit");

    const auto start = std::chrono::steady_clock::now();
    std::uint32_t overcommitRefused = 0;
    for (std::uint32_t index = 0; index < overcommitAttempts; ++index) {
        Texture texture(*renderer, nextResourceId++);
        if (texture.result() == 0)
            continue;
        require(isOutOfMemory(texture.result()),
            "an overcommit was refused for a reason other than memory");
        ++overcommitRefused;
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::fprintf(stderr,
        "overcommit: %zu textures held; %u of %u requests past the full "
        "budget were refused in %.2f s\n",
        held.size(), overcommitRefused, overcommitAttempts, seconds);
    require(overcommitRefused > overcommitAttempts / 2,
        "the budget did not hold against an overcommit");
    require(seconds <= refusalSecondsAllowed * overcommitAttempts,
        "refusing an overcommit takes too long: every refusal pays for a "
        "full reclaim");

    // Release the later half; the earlier textures still hold their pixels.
    const std::size_t kept = held.size() / 2;
    held.resize(kept);
    std::uint32_t checked = 0;
    for (std::size_t index = 0; index < kept; ++index) {
        if (patterns[index] == 0)
            continue;
        require(held[index]->readsAs(patterns[index]),
            "a texture the guest held lost its pixels to a refusal");
        ++checked;
    }
    require(checked > 0, "no held texture was written, so nothing was checked");

    // Released, the room is whole again.
    held.clear();
    for (std::uint32_t index = 0; index < 4; ++index) {
        Texture texture(*renderer, nextResourceId++);
        require(texture.stores(static_cast<std::uint8_t>(index + 201)),
            "a texture made after the guest released its memory has no "
            "storage");
    }
    std::fputs("Reclaim and retry: ok\n", stderr);
    return 0;
}
