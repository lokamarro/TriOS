// SPDX-License-Identifier: GPL-2.0-or-later

#include "host_gpu_memory.hpp"

#import <Metal/Metal.h>
#include <mach/mach.h>

#include <algorithm>

namespace uml::platform {

std::uint64_t hostGpuAllocatedBytes() noexcept
{
    /* Apple silicon has one GPU and Metal hands every caller the same device
     * object, so its counter covers the renderer, the host driver and the
     * presentation path alike. */
    static id<MTLDevice> const device = MTLCreateSystemDefaultDevice();
    return device == nil ? 0 : static_cast<std::uint64_t>(device.currentAllocatedSize);
}

std::uint64_t hostGraphicsFootprintBytes() noexcept
{
    task_vm_info_data_t info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
            reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS ||
        count < TASK_VM_INFO_COUNT)
        return 0;
    const std::int64_t bytes = info.ledger_tag_graphics_footprint +
        info.ledger_tag_graphics_footprint_compressed;
    return static_cast<std::uint64_t>(std::max<std::int64_t>(bytes, 0));
}

} // namespace uml::platform
