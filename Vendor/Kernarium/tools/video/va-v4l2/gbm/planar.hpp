// SPDX-License-Identifier: GPL-2.0-or-later
// Planar buffer allocation for a Mesa gbm device that has none.
//
// Debian's libgbm looks every format up in a table of colour formats, so
// NV12, P010 and the three-plane layouts fail with EINVAL before they reach a
// driver, and a video pipeline (Chromium's GpuMemoryBuffer for a frame is
// exactly gbm_bo_create(NV12)) has nowhere to put its frames. This adapter
// serves them on the device Mesa made, from what Mesa already allocates well:
// one buffer object per plane in the format the plane really has (R8 and GR88
// for NV12), each its own resource of the guest GPU with its own dma-buf, the
// way Mesa's DRI frontend describes a planar image when it must. Every other
// call goes to Mesa's own function, so a compositor's scanout and rendering
// buffers are the buffers Mesa made.
#pragma once

#include <cstdint>

#include "abi.hpp"

namespace uml::gbm {

struct PlaneSpec {
    // The format of the plane's own buffer object.
    std::uint32_t format;
    // Log2 of the factor by which the plane is smaller than the image, per axis.
    unsigned shift;
};

struct PlanarFormat {
    std::uint32_t fourcc;
    unsigned planes;
    PlaneSpec plane[3];
};

// The layout this adapter allocates for a FourCC, or null for any other.
const PlanarFormat* planarFormat(std::uint32_t fourcc) noexcept;

// Serves the planar formats on `device`, which an inner backend just made, by
// replacing entries of its function table. A device keeps working exactly as
// before when the adapter declines: when the inner backend already allocates
// NV12 itself, or the device differs from the one the adapter already serves.
// Returns whether planar allocation is now on.
bool adapt(abi::Device* device) noexcept;

}
