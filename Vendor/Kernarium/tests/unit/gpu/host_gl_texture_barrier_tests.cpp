// SPDX-License-Identifier: GPL-2.0-or-later

// A guest that renders into a texture and then samples it in the draws that
// follow, as Skia does for a blend it cannot express in the fixed-function
// stage, separates the two with a texture barrier. Whatever the guest drew
// before the barrier, a clear included, must be what those draws read. The host
// used to leave a clear it had not applied yet for the next render pass to
// apply as its load operation: on a GPU that keeps the attachment in tile
// memory until the pass ends the draw sampled the texture as it was before the
// clear, and every blend of a surface the guest had just cleared was wrong.
//
// The contract under test: a draw after a clear and a texture barrier reads the
// cleared value from the texture it also renders into.

#include "host_gl_stream_support.hpp"
#include "host_gl_test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <vector>

namespace {

using namespace uml::gpu::testing::stream;
using uml::gpu::testing::skipWithoutRenderer;

constexpr std::uint32_t commandClear = 7;
constexpr std::uint32_t commandTextureBarrier = 39;
constexpr std::uint32_t clearColor0 = 4;
constexpr std::uint32_t samplerBarrier = 1;
constexpr std::uint32_t size = 4;
constexpr std::uint32_t targetId = 5;

// Writes one 8-bit step more than the texel it samples.
constexpr std::string_view stepFragmentShader =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL TEMP[0]\n"
    "IMM[0] FLT32 {0.003921569, 0.003921569, 0.003921569, 0.003921569}\n"
    "  0: TEX TEMP[0], IN[0], SAMP[0], 2D\n"
    "  1: ADD OUT[0], TEMP[0], IMM[0]\n"
    "  2: END\n";

class Target final {
public:
    Target(VirglRenderer& renderer, Pipeline& pipeline)
        : renderer_ {renderer}
        , image_(renderer, targetId, pipeTexture2d, virglFormatR8g8b8a8Unorm,
              virglBindRenderTarget | virglBindSamplerView, size, size, 0,
              static_cast<std::size_t>(size) * size * 4U)
    {
        const VirglRenderer::Box box {
            .width = size, .height = size, .depth = 1};
        require(renderer.transferResource(
                    true, targetId, contextId, 0, size * 4U, 0, box, 0) == 0,
            "the target could not be written");
        const std::uint32_t view = pipeline.createView(
            targetId, virglFormatR8g8b8a8Unorm, 0);
        const std::uint32_t surface = pipeline.newHandle();
        Commands commands;
        commands.add(commandCreateObject, objectSurface,
            {surface, targetId, virglFormatR8g8b8a8Unorm, 0, 0});
        commands.add(commandSetFramebufferState, 0, {1, 0, surface});
        const float half = static_cast<float>(size) / 2.0F;
        commands.add(commandSetViewportState, 0,
            {0, bits(half), bits(half), bits(0.5F), bits(half), bits(half),
                bits(0.5F)});
        commands.add(commandSetSamplerViews, 0, {shaderFragment, 0, view});
        require(commands.submit(renderer) == 0, "the target could not be set");
    }

    // Clears the target to `value`, separates the clear from the draw with a
    // texture barrier, draws, and returns what the draw wrote.
    [[nodiscard]] std::vector<std::uint8_t> clearBarrierDraw(int value)
    {
        const float level = static_cast<float>(value) / 255.0F;
        Commands commands;
        commands.add(commandClear, 0,
            {clearColor0, bits(level), bits(level), bits(level), bits(level),
                0, 0, 0});
        commands.add(commandTextureBarrier, 0, {samplerBarrier});
        commands.add(commandDrawVbo, 0,
            {0, 4, primitiveTriangleStrip, 0, 1, 0, 0, 0, 0, 0, 3, 0});
        require(commands.submit(renderer_) == 0, "the clear and draw failed");
        return read();
    }

    // A draw, a barrier and another draw: the second reads what the first wrote.
    [[nodiscard]] std::vector<std::uint8_t> drawBarrierDraw()
    {
        Commands commands;
        for (int draw = 0; draw < 2; ++draw) {
            commands.add(commandTextureBarrier, 0, {samplerBarrier});
            commands.add(commandDrawVbo, 0,
                {0, 4, primitiveTriangleStrip, 0, 1, 0, 0, 0, 0, 0, 3, 0});
        }
        require(commands.submit(renderer_) == 0, "the draws failed");
        return read();
    }

    // Sets the target to `value` the way a guest does, by writing its texels.
    void write(int value)
    {
        std::fill(image_.bytes().begin(), image_.bytes().end(),
            static_cast<std::uint8_t>(value));
        const VirglRenderer::Box box {
            .width = size, .height = size, .depth = 1};
        require(renderer_.transferResource(
                    true, targetId, contextId, 0, size * 4U, 0, box, 0) == 0,
            "the target could not be written");
    }

private:
    [[nodiscard]] std::vector<std::uint8_t> read()
    {
        require(renderer_.createFenceAndWait(contextId, 0, ++fence_, nullptr) ==
                0,
            "the draw did not complete");
        std::vector<std::uint8_t> texels(static_cast<std::size_t>(size) * size * 4U);
        require(renderer_.readResourceRectangle(
                    targetId, texels, 0, 0, size, size) == 0,
            "the target could not be read");
        return texels;
    }

    VirglRenderer& renderer_;
    Image image_;
    std::uint64_t fence_ {};
};

void expectEvery(const std::vector<std::uint8_t>& texels, int channelValue,
    const char* what)
{
    for (std::size_t index = 0; index < texels.size(); ++index) {
        // The alpha channel is stepped like the others.
        if (texels[index] != channelValue) {
            std::fprintf(stderr, "%s: byte %zu is %u, wanted %d\n", what,
                index, texels[index], channelValue);
            require(false, "the draw did not read what was drawn before the barrier");
        }
    }
}

} // namespace

int main()
{
    std::unique_ptr<VirglRenderer> renderer;
    try {
        renderer = std::make_unique<VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }
    require(renderer->createContext(contextId, virgl2CapsetId, "barrier") == 0,
        "the guest context could not be created");

    Pipeline pipeline(*renderer, stepFragmentShader);
    Target target(*renderer, pipeline);

    // Every value differs from the step after the one before it, so a draw that
    // read what the last draw wrote instead of the clear could not match.
    for (int iteration = 0; iteration < 12; ++iteration) {
        const int value = (iteration * 7) % 200 + 20;
        expectEvery(target.clearBarrierDraw(value), value + 1, "clear");
    }

    // Draws that were never a clear were ordered before, and still are.
    target.write(40);
    expectEvery(target.drawBarrierDraw(), 42, "draw");

    std::fputs("Texture barrier after a clear: ok\n", stderr);
    return 0;
}
