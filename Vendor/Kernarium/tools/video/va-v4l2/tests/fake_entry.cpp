// SPDX-License-Identifier: GPL-2.0-or-later
// A libva backend over the in-memory encoder node, loaded by libva the way the
// real driver is. It lets the guest-side probes run end to end - libva's own
// loader, the driver's entry point, the VA-API calls - on a build host that has
// no Virtio video device.
#include <va/va_backend.h>

#include <cstdlib>

#include "../driver.hpp"
#include "fake_v4l2.hpp"

extern "C" __attribute__((visibility("default"))) VAStatus __vaDriverInit_1_22(VADriverContextP context)
{
    using namespace uml::va;
    // The node is the fake's, and nothing else is searched for.
    setenv("UML_VA_V4L2_DEVICE", "/dev/uml-no-decoder", 0);
    setenv("UML_VA_V4L2_ENCODER_DEVICE", test::FakeEncoderNode::kPath, 0);
    setenv("UML_VA_V4L2_SURFACES", "memory", 0);
    static test::FakeEncoderNode node;
    static DescriptorPool pool(node);
    return initializeDriver(context, pool);
}
