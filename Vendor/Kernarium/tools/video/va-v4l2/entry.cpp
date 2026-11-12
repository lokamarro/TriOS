// SPDX-License-Identifier: GPL-2.0-or-later
// The one symbol libva looks for in a driver module.
#include "driver.hpp"

extern "C" VAStatus __vaDriverInit_1_22(VADriverContextP ctx);

extern "C" VAStatus __vaDriverInit_1_22(VADriverContextP ctx)
{
    return uml::va::initializeDriver(ctx, uml::va::DescriptorPool::process());
}
