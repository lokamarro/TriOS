// SPDX-License-Identifier: GPL-2.0-or-later

// Without a host allocator a guest keeps a YUV picture as the bytes of one R8
// image: the luma rows, then the chroma rows, in the layout Android defines. A
// shader that samples the picture reads the luma plane and the chroma plane or
// planes as separate textures, and the host used to have one texture for the
// whole image, so luma came back as the picture squeezed into two thirds of the
// frame and every chroma view read what luma did. Every decoder that plays
// into a GL or SurfaceView view showed stripes.
//
// The contract under test: a resource created with a packed YUV layout and the
// picture's width samples as the planes it holds, through the plane number a
// sampler view carries, and keeps doing so after the guest rewrites some of its
// rows; a resource created without the layout is the plain texture it always
// was; and a layout that does not describe a picture is refused.
//
// A biplanar picture keeps its pixels only in its plane textures. A transfer
// of any box of it, from any byte, writes or reads the planes and none of the
// padding of the rows, and every other use of the image (a copy, a surface)
// first gives it the R8 storage of a plain image back, with the planes' pixels.

#include "host_gl_stream_support.hpp"
#include "host_gl_test_support.hpp"

#include <virgl_hw.h>

#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace uml::gpu::testing::stream;
using uml::gpu::testing::skipWithoutRenderer;

// A YUV 4:2:0 picture and the image the guest allocator keeps it in.
struct Picture {
    bool biplanar;
    std::uint32_t width;
    std::uint32_t height;
    // The bytes of a luma row when the allocator's alignment is not the one in
    // use, which a guest may choose.
    std::uint32_t pitch {};

    [[nodiscard]] std::uint32_t stride() const
    {
        return pitch ? pitch : alignUp(width, biplanar ? 16U : 32U);
    }
    // Whether the byte at column x of image row y is one a biplanar picture
    // keeps no texel for: past its width in a luma row, past its last
    // interleaved pair in a chroma row.
    [[nodiscard]] bool padding(std::uint32_t x, std::uint32_t y) const
    {
        return y < height ? x >= width : x >= 2U * chromaWidth();
    }
    [[nodiscard]] std::uint32_t chromaRows() const { return (height + 1) / 2; }
    [[nodiscard]] std::uint32_t chromaWidth() const { return (width + 1) / 2; }
    [[nodiscard]] std::uint32_t rows() const { return height + chromaRows(); }
    [[nodiscard]] std::uint32_t planes() const { return biplanar ? 2 : 3; }
    [[nodiscard]] std::uint32_t flags() const
    {
        return (biplanar ? VIRGL_RESOURCE_FLAG_YUV_BIPLANAR
                         : VIRGL_RESOURCE_FLAG_YUV_TRIPLANAR) |
               (width << VIRGL_RESOURCE_YUV_WIDTH_SHIFT);
    }
    [[nodiscard]] std::uint32_t planeWidth(std::uint32_t plane) const
    {
        return plane == 0 ? width : chromaWidth();
    }
    [[nodiscard]] std::uint32_t planeHeight(std::uint32_t plane) const
    {
        return plane == 0 ? height : chromaRows();
    }
    [[nodiscard]] std::uint32_t planeFormat(std::uint32_t plane) const
    {
        return plane == 1 && biplanar ? virglFormatR8g8Unorm
                                      : virglFormatR8Unorm;
    }

    // The two channels a plane texel holds, read from the bytes of the image
    // the way Android lays the picture out in them.
    [[nodiscard]] std::array<std::uint8_t, 2> texel(
        const std::vector<std::uint8_t>& image,
        std::uint32_t plane,
        std::uint32_t x,
        std::uint32_t y) const
    {
        const std::size_t luma = static_cast<std::size_t>(stride()) * height;
        if (plane == 0)
            return {image[static_cast<std::size_t>(y) * stride() + x], 0};
        if (biplanar) {
            const std::size_t at =
                luma + static_cast<std::size_t>(y) * stride() + 2U * x;
            return {image[at], image[at + 1]};
        }
        const std::size_t chromaStride = stride() / 2U;
        const std::size_t at = luma +
            (plane - 1U) * chromaStride * chromaRows() +
            static_cast<std::size_t>(y) * chromaStride + x;
        return {image[at], 0};
    }
};

// Every byte of the image different from its neighbours, so a texel read from
// the wrong place cannot match.
void fill(std::vector<std::uint8_t>& image, std::uint32_t seed)
{
    for (std::size_t index = 0; index < image.size(); ++index)
        image[index] = static_cast<std::uint8_t>(index * 37U + seed);
}


// Draws every plane of the picture through its view and compares each texel
// with the one the layout puts there.
void checkPlanes(
    Pipeline& pipeline,
    const Picture& picture,
    const std::vector<std::uint8_t>& image,
    const std::array<std::uint32_t, 3>& views,
    const char* when)
{
    for (std::uint32_t plane = 0; plane < picture.planes(); ++plane) {
        const std::uint32_t width = picture.planeWidth(plane);
        const std::uint32_t height = picture.planeHeight(plane);
        const auto texels = pipeline.sample(views[plane], width, height);
        std::uint32_t wrong = 0;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto expected = picture.texel(image, plane, x, y);
                const std::uint8_t* const actual =
                    &texels[(static_cast<std::size_t>(y) * width + x) * 4U];
                const bool pair = plane == 1 && picture.biplanar;
                if (actual[0] != expected[0] ||
                    (pair && actual[1] != expected[1])) {
                    if (wrong++ < 4)
                        std::fprintf(stderr,
                            "%s %s %ux%u plane %u texel (%u,%u): got %u,%u "
                            "wanted %u,%u\n",
                            when, picture.biplanar ? "biplanar" : "triplanar",
                            picture.width, picture.height, plane, x, y,
                            actual[0], actual[1], expected[0], expected[1]);
                }
            }
        }
        require(wrong == 0, "a plane did not sample as the layout says");
    }
}

void checkPicture(VirglRenderer& renderer, Pipeline& pipeline, Picture picture)
{
    constexpr std::uint32_t imageId = 3;
    Image image(renderer, imageId, pipeTexture2d, virglFormatR8Unorm,
        virglBindSamplerView, picture.stride(), picture.rows(), picture.flags(),
        static_cast<std::size_t>(picture.stride()) * picture.rows());
    fill(image.bytes(), 11);
    image.write(picture.stride(), 0, picture.rows());

    // The views are made after the first write: a triplanar image makes its
    // planes with the first of them, from the image as it is then, and a
    // biplanar one wrote them directly.
    std::array<std::uint32_t, 3> views {};
    for (std::uint32_t plane = 0; plane < picture.planes(); ++plane) {
        views[plane] = pipeline.createView(
            imageId, picture.planeFormat(plane), plane);
    }
    checkPlanes(pipeline, picture, image.bytes(), views, "first write");

    // Rewriting some rows changes the planes they feed, in the same views.
    // Rows are picked to cut across the chroma rows of a triplanar image, two
    // of which share an image row.
    const std::uint32_t first = picture.height - 1;
    const std::uint32_t count = picture.rows() - first - 1;
    for (std::uint32_t row = first; row < first + count; ++row) {
        for (std::uint32_t column = 0; column < picture.stride(); ++column)
            image.bytes()[static_cast<std::size_t>(row) * picture.stride() +
                column] = static_cast<std::uint8_t>(~(row * 53U + column * 7U));
    }
    image.write(picture.stride(), first, count);
    checkPlanes(pipeline, picture, image.bytes(), views, "partial rewrite");
}

// A rectangle of an image, in bytes.
struct Rectangle {
    std::uint32_t x;
    std::uint32_t y;
    std::uint32_t width;
    std::uint32_t height;
};

// Gives every byte of the rectangle a value the rest of the image lacks.
void fillRectangle(
    std::vector<std::uint8_t>& image,
    std::uint32_t stride,
    const Rectangle& box,
    std::uint32_t seed)
{
    for (std::uint32_t row = 0; row < box.height; ++row) {
        for (std::uint32_t column = 0; column < box.width; ++column) {
            image[static_cast<std::size_t>(box.y + row) * stride + box.x +
                column] =
                static_cast<std::uint8_t>((row * 131U + column * 17U + seed) ^
                    0x5aU);
        }
    }
}

// Reads `box` of the image back into poisoned guest storage and compares it
// with `model`: the bytes the picture keeps come back as written, and every
// other byte, the padding of a row and whatever lies outside the box, is left
// as the poison it was. A plain image returns its padding too.
void checkRead(
    Image& image,
    const Picture& picture,
    const std::vector<std::uint8_t>& model,
    const Rectangle& box,
    bool nameStride,
    bool keepsPadding,
    const char* what)
{
    constexpr std::uint8_t poison = 0xa5;
    const std::uint32_t stride = picture.stride();
    std::fill(image.bytes().begin(), image.bytes().end(), poison);
    require(image.readBox(
                stride, box.x, box.y, box.width, box.height, nameStride) == 0,
        "the box could not be read");
    std::uint32_t wrong = 0;
    for (std::uint32_t y = 0; y < picture.rows(); ++y) {
        for (std::uint32_t x = 0; x < stride; ++x) {
            const bool inside = x >= box.x && x < box.x + box.width &&
                y >= box.y && y < box.y + box.height;
            const std::size_t at = static_cast<std::size_t>(y) * stride + x;
            const bool kept = !picture.padding(x, y);
            std::uint8_t expected = poison;
            if (inside && kept)
                expected = model[at];
            else if (inside && keepsPadding)
                expected = model[at];
            if (image.bytes()[at] != expected && wrong++ < 4) {
                std::fprintf(stderr,
                    "%s %ux%u byte (%u,%u): got %u wanted %u\n", what,
                    picture.width, picture.height, x, y, image.bytes()[at],
                    expected);
            }
        }
    }
    require(wrong == 0, "a read did not return the bytes the picture keeps");
}

// A biplanar picture's boxes: a transfer of any rectangle of the image,
// starting at any byte and ending at any byte, writes the pixels it carries
// and no others, and a read returns them.
void checkBoxes(VirglRenderer& renderer, Pipeline& pipeline, Picture picture)
{
    constexpr std::uint32_t imageId = 3;
    const std::uint32_t stride = picture.stride();
    const std::uint32_t height = picture.height;
    const std::uint32_t rows = picture.rows();
    Image image(renderer, imageId, pipeTexture2d, virglFormatR8Unorm,
        virglBindSamplerView, stride, rows, picture.flags(),
        static_cast<std::size_t>(stride) * rows);
    fill(image.bytes(), 21);
    image.write(stride, 0, rows);
    std::array<std::uint32_t, 3> views {};
    for (std::uint32_t plane = 0; plane < picture.planes(); ++plane) {
        views[plane] = pipeline.createView(
            imageId, picture.planeFormat(plane), plane);
    }
    checkPlanes(pipeline, picture, image.bytes(), views, "whole image");

    struct Write {
        Rectangle box;
        bool nameStride;
        const char* what;
    };
    const std::array<Write, 9> writes {{
        {{1, 0, 7, 2}, true, "luma interior"},
        {{3, height - 1, 5, 2}, true, "luma and chroma from an odd byte"},
        {{2, height, 4, 1}, true, "whole pairs"},
        {{2, height, 3, 1}, true, "a cut last pair"},
        {{3, height, 4, 1}, true, "a cut first pair"},
        {{5, height + 1, 1, 1}, true, "one byte of a pair"},
        {{picture.width - 1, 0, stride - picture.width + 1, rows}, true,
            "the right edge with its padding"},
        {{picture.width, 0, stride - picture.width, height}, true,
            "padding only"},
        {{0, 1, stride, 3}, false, "rows with the pitch left to the renderer"},
    }};
    std::uint32_t seed = 40;
    for (const Write& write : writes) {
        fillRectangle(image.bytes(), stride, write.box, seed++);
        image.writeBox(stride, write.box.x, write.box.y, write.box.width,
            write.box.height, write.nameStride);
        checkPlanes(pipeline, picture, image.bytes(), views, write.what);
    }

    // What the host holds comes back through a read of any box.
    const std::vector<std::uint8_t> model = image.bytes();
    const std::array<Write, 6> reads {{
        {{0, 0, stride, rows}, true, "read of the whole image"},
        {{3, height - 1, 5, 2}, true, "read from an odd byte"},
        {{2, height, 3, 2}, true, "read of a cut last pair"},
        {{5, height + 1, 1, 1}, true, "read of one byte"},
        {{picture.width - 1, 0, stride - picture.width + 1, rows}, true,
            "read of the right edge"},
        {{0, 0, stride, rows}, false, "read with the pitch left to the renderer"},
    }};
    for (const Write& read : reads)
        checkRead(image, picture, model, read.box, read.nameStride, false,
            read.what);
}

// The commands that use an image whole: a copy of it and a surface on it.
constexpr std::uint32_t commandResourceCopyRegion = 17;

enum class Use { copy, surface };

// A use of the image other than a transfer gives a planes-only picture the R8
// storage of a plain image back, filled from the planes: the use sees the
// picture, the views keep sampling it, and from then on the image is a plain
// one that keeps every byte it is sent and refreshes its planes from them.
void checkMaterialized(
    VirglRenderer& renderer,
    Pipeline& pipeline,
    Picture picture,
    Use use)
{
    constexpr std::uint32_t imageId = 3;
    constexpr std::uint32_t copyId = 4;
    const std::uint32_t stride = picture.stride();
    const std::uint32_t rows = picture.rows();
    const std::size_t size = static_cast<std::size_t>(stride) * rows;
    Image image(renderer, imageId, pipeTexture2d, virglFormatR8Unorm,
        virglBindSamplerView, stride, rows, picture.flags(), size);
    fill(image.bytes(), 77);
    image.write(stride, 0, rows);
    std::array<std::uint32_t, 3> views {};
    for (std::uint32_t plane = 0; plane < picture.planes(); ++plane) {
        views[plane] = pipeline.createView(
            imageId, picture.planeFormat(plane), plane);
    }
    // Half of the rows are rewritten, so the host's pixels differ from the
    // bytes the image was created with.
    fillRectangle(image.bytes(), stride, {2, 1, stride - 3, rows - 2}, 9);
    image.writeBox(stride, 2, 1, stride - 3, rows - 2);
    std::vector<std::uint8_t> model = image.bytes();

    Commands commands;
    if (use == Use::copy) {
        Image copy(renderer, copyId, pipeTexture2d, virglFormatR8Unorm,
            virglBindSamplerView, stride, rows, 0, size);
        commands.add(commandResourceCopyRegion, 0,
            {copyId, 0, 0, 0, 0, imageId, 0, 0, 0, 0, stride, rows, 1});
        require(commands.submit(renderer) == 0, "the copy failed");
        // The copy has every byte the planes hold, and zeros for the padding
        // the planes cannot return.
        const std::uint32_t bad = [&] {
            require(copy.readBox(stride, 0, 0, stride, rows) == 0,
                "the copy could not be read");
            std::uint32_t count = 0;
            for (std::uint32_t y = 0; y < rows; ++y) {
                for (std::uint32_t x = 0; x < stride; ++x) {
                    const std::size_t at = static_cast<std::size_t>(y) * stride + x;
                    const std::uint8_t expected =
                        picture.padding(x, y) ? 0 : model[at];
                    if (copy.bytes()[at] != expected && count++ < 4) {
                        std::fprintf(stderr,
                            "copy %ux%u byte (%u,%u): got %u wanted %u\n",
                            picture.width, picture.height, x, y,
                            copy.bytes()[at], expected);
                    }
                }
            }
            return count;
        }();
        require(bad == 0, "a copy of a packed image did not hold its pixels");
    } else {
        commands.add(commandCreateObject, objectSurface,
            {pipeline.newHandle(), imageId, virglFormatR8Unorm, 0, 0});
        require(commands.submit(renderer) == 0, "the surface failed");
    }

    // Sampled through the views that were made before.
    checkPlanes(pipeline, picture, model, views, "after the use");
    // The image is the plain one now: a read returns the padding it holds, and
    // so does a read of a box that starts at an odd byte.
    std::vector<std::uint8_t> plain = model;
    for (std::uint32_t y = 0; y < rows; ++y) {
        for (std::uint32_t x = 0; x < stride; ++x) {
            if (picture.padding(x, y))
                plain[static_cast<std::size_t>(y) * stride + x] = 0;
        }
    }
    checkRead(image, picture, plain, {0, 0, stride, rows}, true, true,
        "plain read of the whole image");
    checkRead(image, picture, plain, {3, picture.height, 4, 1}, true, true,
        "plain read of a box");

    // A write now lands in the image and follows into the planes, padding and
    // all.
    image.bytes() = plain;
    fillRectangle(image.bytes(), stride, {picture.width - 1, 0,
                      stride - picture.width + 1, rows}, 123);
    image.writeBox(stride, picture.width - 1, 0, stride - picture.width + 1,
        rows);
    checkPlanes(pipeline, picture, image.bytes(), views, "write after the use");
    model = image.bytes();
    checkRead(image, picture, model, {0, 0, stride, rows}, true, true,
        "plain read after a write");
}

// A resource created without a layout is the plain texture it always was, and
// a plane number on a view of it selects nothing.
void checkPlainImage(VirglRenderer& renderer, Pipeline& pipeline)
{
    constexpr std::uint32_t imageId = 3;
    constexpr std::uint32_t width = 16;
    constexpr std::uint32_t height = 9;
    Image image(renderer, imageId, pipeTexture2d, virglFormatR8Unorm,
        virglBindSamplerView, width, height, 0, width * height);
    fill(image.bytes(), 5);
    image.write(width, 0, height);
    for (std::uint32_t plane = 0; plane < 2; ++plane) {
        const std::uint32_t view =
            pipeline.createView(imageId, virglFormatR8Unorm, plane);
        const auto texels = pipeline.sample(view, width, height);
        for (std::size_t index = 0; index < width * height; ++index)
            require(texels[index * 4U] == image.bytes()[index],
                "a resource without a layout did not sample as one texture");
    }
}

// What the renderer will not take for a picture.
void checkRefused(VirglRenderer& renderer)
{
    constexpr std::uint32_t id = 3;
    const Picture picture {true, 10, 6};
    const std::uint32_t good = picture.flags();
    const auto create = [&](std::uint32_t stride, std::uint32_t rows,
                            std::uint32_t flags) {
        const int result = Image::create(renderer, id, pipeTexture2d,
            virglFormatR8Unorm, virglBindSamplerView, stride, rows, flags);
        if (result == 0)
            renderer.unrefResource(id);
        return result;
    };

    require(create(16, 9, good) == 0, "a valid picture was refused");
    require(create(16, 10, good) != 0,
        "an image that is not luma and chroma rows was accepted");
    require(create(16, 9,
                good | VIRGL_RESOURCE_FLAG_YUV_TRIPLANAR) != 0,
        "an image with both layouts was accepted");
    require(create(16, 9, VIRGL_RESOURCE_FLAG_YUV_BIPLANAR) != 0,
        "a picture without a width was accepted");
    require(create(16, 9, VIRGL_RESOURCE_FLAG_YUV_BIPLANAR |
                (17U << VIRGL_RESOURCE_YUV_WIDTH_SHIFT)) != 0,
        "a picture wider than its luma row was accepted");
    require(create(10, 9, VIRGL_RESOURCE_FLAG_YUV_BIPLANAR |
                (11U << VIRGL_RESOURCE_YUV_WIDTH_SHIFT)) != 0,
        "a picture wider than its luma row was accepted");
    // An odd width in a luma row of exactly that width leaves its last chroma
    // pair a byte short.
    require(create(9, 9, VIRGL_RESOURCE_FLAG_YUV_BIPLANAR |
                (9U << VIRGL_RESOURCE_YUV_WIDTH_SHIFT)) != 0,
        "chroma pairs that do not fit in a luma row were accepted");
    // The chroma rows are half a luma row wide, so the luma row is even.
    require(create(17, 9, VIRGL_RESOURCE_FLAG_YUV_TRIPLANAR |
                (17U << VIRGL_RESOURCE_YUV_WIDTH_SHIFT)) != 0,
        "chroma planes of half an odd luma row were accepted");
    require(create(16, 9, good | VIRGL_RESOURCE_FLAG_MAP_COHERENT) != 0,
        "a layout together with another resource flag was accepted");
    require(create(16, 9, 10U << VIRGL_RESOURCE_YUV_WIDTH_SHIFT) != 0,
        "a width without a layout was accepted");
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

    const auto capset = renderer->capset(virgl2CapsetId).data;
    virgl_caps_v2 caps {};
    require(capset.size() >=
            offsetof(virgl_caps_v2, capability_bits_v2) +
                sizeof(caps.capability_bits_v2),
        "the capset is too short to hold the capability bits");
    std::memcpy(&caps, capset.data(), std::min(capset.size(), sizeof(caps)));
    require((caps.capability_bits_v2 & VIRGL_CAP_V2_PACKED_YUV) != 0,
        "the renderer does not advertise packed YUV images");

    require(renderer->createContext(contextId, virgl2CapsetId, "yuv") == 0,
        "the guest context could not be created");

    checkRefused(*renderer);

    Pipeline pipeline(*renderer);
    checkPlainImage(*renderer, pipeline);
    // An even picture and one whose odd sizes put the second chroma plane in
    // the middle of an image row, in each layout.
    for (const bool biplanar : {true, false}) {
        checkPicture(*renderer, pipeline, Picture {biplanar, 10, 6});
        checkPicture(*renderer, pipeline, Picture {biplanar, 9, 5});
        checkPicture(*renderer, pipeline, Picture {biplanar, 33, 17});
    }

    // The pixels of a biplanar picture live in its planes, and any box of it,
    // at any byte and at any pitch, goes to them. A pitch of 19 bytes for a
    // picture 9 wide leaves the interleaved rows an odd number of bytes apart.
    for (const Picture& picture : {Picture {true, 10, 6}, Picture {true, 9, 5},
             Picture {true, 33, 17}, Picture {true, 9, 5, 19}}) {
        checkBoxes(*renderer, pipeline, picture);
        checkMaterialized(*renderer, pipeline, picture, Use::copy);
        checkMaterialized(*renderer, pipeline, picture, Use::surface);
    }

    std::fputs("Packed YUV planes: ok\n", stderr);
    return 0;
}
