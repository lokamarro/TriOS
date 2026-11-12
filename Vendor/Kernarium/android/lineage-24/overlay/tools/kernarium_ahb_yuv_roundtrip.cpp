// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Round-trips a YUV hardware buffer through the host copy of its pixels.
 *
 * A buffer of NV12, P010 or YV12 is written whole through one lock, then a
 * rectangle of it is rewritten through a second lock of only that rectangle,
 * and the whole buffer is read back through a third lock. The unlock of a
 * partial lock sends the rectangle to the host and the next lock fetches it, so
 * a buffer whose layout the guest and the host read differently, a rectangle
 * whose chroma samples are sent from the wrong place, or a row too short for the
 * chroma samples of an odd width, shows as a mismatch. Every plane is compared
 * sample by sample: the rectangle holds the second pattern, the rest the first.
 *
 * Usage: kernarium_ahb_yuv_roundtrip [nv12|p010|yv12 [width height x y w h]]
 * With no argument it runs rectangles of several sizes for each format,
 * including widths and heights that are odd. A format the allocator does not
 * offer is reported as skipped. The exit status is 0 when every case that could
 * be allocated passed.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <android/hardware_buffer.h>

namespace {

// HAL_PIXEL_FORMAT_YV12, which the NDK has no name for, and the formats it has.
constexpr uint32_t kYv12 = 0x32315659;
constexpr uint32_t kP010 = AHARDWAREBUFFER_FORMAT_YCbCr_P010;
constexpr uint32_t kNv12 = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;

struct FormatCase {
    const char* name;
    uint32_t ahbFormat;
    bool tenBit;
};

const FormatCase kFormats[] = {
        {"nv12", kNv12, false},
        {"p010", kP010, true},
        {"yv12", kYv12, false},
};

struct Case {
    int width, height, x, y, w, h;
};

// The full buffer, a rectangle that starts and ends mid-pair, one of a single
// row or column, one at the far corner, and the odd widths and heights whose
// chroma rows are longer than half the luma row rounded down.
const Case kCases[] = {
        {640, 360, 0, 0, 640, 360},
        {640, 360, 320, 180, 320, 180},
        {642, 362, 101, 51, 201, 99},
        {640, 361, 1, 1, 639, 359},
        {334, 177, 15, 9, 7, 5},
        {334, 177, 331, 175, 3, 2},
        {333, 177, 0, 0, 333, 177},
        {333, 177, 1, 3, 331, 171},
        {333, 177, 332, 176, 1, 1},
        {641, 361, 7, 13, 634, 340},
        {64, 64, 1, 0, 3, 64},
        {64, 64, 0, 1, 64, 3},
        {1921, 1081, 7, 13, 1500, 900},
        {1921, 1081, 1920, 1080, 1, 1},
        {1280, 720, 3, 5, 1001, 501},
};

// The two patterns. A sample is 8 bits in NV12 and YV12 and the top 10 of 16
// in P010, where the low six bits must survive as zero.
uint16_t patternA(int plane, int x, int y) {
    return static_cast<uint16_t>(((plane * 85 + x * 7 + y * 13 + 1) & 0x3ff) << 6);
}

uint16_t patternB(int plane, int x, int y) {
    return static_cast<uint16_t>((((0x80 ^ (plane * 51 + x * 3 + y * 5)) & 0x3ff) << 6) | 0x20);
}

struct Sampler {
    bool tenBit;

    void put(uint8_t* p, uint16_t v) const {
        if (tenBit) {
            p[0] = v & 0xff;
            p[1] = v >> 8;
        } else {
            p[0] = v >> 8;
        }
    }
    uint16_t get(const uint8_t* p) const {
        return tenBit ? static_cast<uint16_t>(p[0] | p[1] << 8) : static_cast<uint16_t>(p[0] << 8);
    }
    uint16_t mask(uint16_t v) const { return tenBit ? v : static_cast<uint16_t>(v & 0xff00); }
};

uint8_t* sample(const AHardwareBuffer_Planes& p, int plane, int x, int y) {
    return static_cast<uint8_t*>(p.planes[plane].data) + static_cast<size_t>(y) * p.planes[plane].rowStride +
           static_cast<size_t>(x) * p.planes[plane].pixelStride;
}

// Returns 0 on a pass, 1 on a mismatch or a failed lock, -1 when the format cannot be allocated.
int runCase(const FormatCase& format, const Case& c) {
    const Sampler s{format.tenBit};
    AHardwareBuffer_Desc desc;
    memset(&desc, 0, sizeof desc);
    desc.width = c.width;
    desc.height = c.height;
    desc.layers = 1;
    desc.format = format.ahbFormat;
    desc.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    AHardwareBuffer* buffer = nullptr;
    if (AHardwareBuffer_allocate(&desc, &buffer) != 0 || !buffer) return -1;

    const int cw = (c.width + 1) / 2, ch = (c.height + 1) / 2;
    AHardwareBuffer_Planes p;
    memset(&p, 0, sizeof p);
    if (AHardwareBuffer_lockPlanes(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &p) != 0) {
        printf("FAIL %s %dx%d: the whole-buffer lock failed\n", format.name, c.width, c.height);
        AHardwareBuffer_release(buffer);
        return 1;
    }
    if (p.planeCount < 3) {
        printf("FAIL %s %dx%d: the lock reports %u planes\n", format.name, c.width, c.height, p.planeCount);
        AHardwareBuffer_unlock(buffer, nullptr);
        AHardwareBuffer_release(buffer);
        return 1;
    }
    for (int y = 0; y < c.height; ++y)
        for (int x = 0; x < c.width; ++x) s.put(sample(p, 0, x, y), patternA(0, x, y));
    for (int plane = 1; plane <= 2; ++plane)
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) s.put(sample(p, plane, x, y), patternA(plane, x, y));
    AHardwareBuffer_unlock(buffer, nullptr);

    // A rectangle of the picture covers the chroma samples of every pair of
    // pixels and rows it touches.
    const int cx0 = c.x / 2, cx1 = (c.x + c.w + 1) / 2, cy0 = c.y / 2, cy1 = (c.y + c.h + 1) / 2;
    ARect rect = {c.x, c.y, c.x + c.w, c.y + c.h};
    if (AHardwareBuffer_lockPlanes(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, &rect, &p) != 0) {
        printf("FAIL %s %dx%d: the rectangle lock failed\n", format.name, c.width, c.height);
        AHardwareBuffer_release(buffer);
        return 1;
    }
    for (int y = c.y; y < c.y + c.h; ++y)
        for (int x = c.x; x < c.x + c.w; ++x) s.put(sample(p, 0, x, y), patternB(0, x, y));
    for (int plane = 1; plane <= 2; ++plane)
        for (int y = cy0; y < cy1; ++y)
            for (int x = cx0; x < cx1; ++x) s.put(sample(p, plane, x, y), patternB(plane, x, y));
    AHardwareBuffer_unlock(buffer, nullptr);

    if (AHardwareBuffer_lockPlanes(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &p) != 0) {
        printf("FAIL %s %dx%d: the read lock failed\n", format.name, c.width, c.height);
        AHardwareBuffer_release(buffer);
        return 1;
    }
    long bad[3] = {0, 0, 0};
    long first[3] = {-1, -1, -1};
    for (int y = 0; y < c.height; ++y)
        for (int x = 0; x < c.width; ++x) {
            const bool inside = x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h;
            const uint16_t want = s.mask(inside ? patternB(0, x, y) : patternA(0, x, y));
            if (want != s.mask(s.get(sample(p, 0, x, y))) && bad[0]++ == 0) first[0] = long(y) * c.width + x;
        }
    for (int plane = 1; plane <= 2; ++plane)
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                const bool inside = x >= cx0 && x < cx1 && y >= cy0 && y < cy1;
                const uint16_t want = s.mask(inside ? patternB(plane, x, y) : patternA(plane, x, y));
                if (want != s.mask(s.get(sample(p, plane, x, y))) && bad[plane]++ == 0)
                    first[plane] = long(y) * cw + x;
            }
    const uint32_t rowStride0 = p.planes[0].rowStride;
    AHardwareBuffer_unlock(buffer, nullptr);
    AHardwareBuffer_release(buffer);

    const int result = bad[0] || bad[1] || bad[2];
    printf("%s %s %dx%d rect %d,%d %dx%d: luma row stride %u, mismatches Y %ld Cb %ld Cr %ld (first %ld %ld %ld)\n",
           result ? "FAIL" : "PASS", format.name, c.width, c.height, c.x, c.y, c.w, c.h, rowStride0, bad[0], bad[1],
           bad[2], first[0], first[1], first[2]);
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    const FormatCase* only = nullptr;
    Case single{};
    bool haveSingle = false;
    if (argc > 1) {
        for (const FormatCase& format : kFormats)
            if (!strcmp(argv[1], format.name)) only = &format;
        if (!only) {
            fprintf(stderr, "usage: %s [nv12|p010|yv12 [width height x y w h]]\n", argv[0]);
            return 2;
        }
        if (argc == 8) {
            single = {atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), atoi(argv[7])};
            haveSingle = true;
        }
    }

    int failed = 0, passed = 0;
    for (const FormatCase& format : kFormats) {
        if (only && only != &format) continue;
        bool skipped = false;
        if (haveSingle) {
            const int r = runCase(format, single);
            if (r < 0) skipped = true;
            else if (r) ++failed;
            else ++passed;
        } else {
            for (const Case& c : kCases) {
                const int r = runCase(format, c);
                if (r < 0) {
                    skipped = true;
                    break;
                }
                if (r) ++failed;
                else ++passed;
            }
        }
        if (skipped) printf("SKIP %s: the allocator does not offer the format\n", format.name);
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
