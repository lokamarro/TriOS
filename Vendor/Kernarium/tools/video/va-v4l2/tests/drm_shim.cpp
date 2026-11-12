// SPDX-License-Identifier: GPL-2.0-or-later
// Preloaded into a probe, it makes libva-drm take any descriptor as a render
// node, so the probes can run through libva's own loader on a build host that
// has no DRM device. The driver ignores the device a display names.
#include <xf86drm.h>

extern "C" __attribute__((visibility("default"))) int drmGetNodeTypeFromFd(int)
{
    return DRM_NODE_RENDER;
}
