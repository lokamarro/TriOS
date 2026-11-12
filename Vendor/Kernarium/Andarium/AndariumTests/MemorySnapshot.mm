// SPDX-License-Identifier: GPL-2.0-or-later
#import "MemorySnapshot.h"

#include <mach/mach.h>
#include <malloc/malloc.h>
#include <cstdint>
#include <dlfcn.h>

NSInteger andarium_test_release_gpu_caches(void)
{
    using Entry = void (*)(void);
    using GetProcAddress = Entry (*)(const char *);
    auto getProcAddress = reinterpret_cast<GetProcAddress>(
        dlsym(RTLD_DEFAULT, "eglGetProcAddress"));
    if (getProcAddress == nullptr) return -1;
    auto release = reinterpret_cast<int (*)(void)>(
        getProcAddress("eglReleaseCachedMemoryMESA"));
    return release == nullptr ? -1 : release();
}

NSDictionary<NSString *, id> *andarium_test_memory_snapshot(void)
{
    task_vm_info_data_t task {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    const kern_return_t taskResult = task_info(mach_task_self(), TASK_VM_INFO,
        reinterpret_cast<task_info_t>(&task), &count);
    malloc_statistics_t heap {};
    malloc_zone_statistics(malloc_default_zone(), &heap);
    NSMutableDictionary *result = [@{
        @"task_info_result": @(taskResult),
        @"footprint": @(task.phys_footprint),
        @"peak_footprint": @(task.ledger_phys_footprint_peak),
        @"remaining": @(task.limit_bytes_remaining),
        @"resident": @(task.resident_size),
        @"virtual": @(task.virtual_size),
        @"graphics": @(task.ledger_tag_graphics_footprint + task.ledger_tag_graphics_footprint_compressed),
        @"malloc_in_use": @(heap.size_in_use),
        @"malloc_allocated": @(heap.size_allocated),
        @"malloc_blocks": @(heap.blocks_in_use),
        @"page_size": @(vm_page_size),
    } mutableCopy];
    NSMutableArray *regions = [NSMutableArray array];
    std::uint64_t heapDirty = 0, scratchDirty = 0, wired = 0;
    vm_address_t address = 0;
    natural_t depth = 0;
    for (;;) {
        vm_size_t size = 0;
        vm_region_submap_info_data_64_t info {};
        count = VM_REGION_SUBMAP_INFO_COUNT_64;
        const auto status = vm_region_recurse_64(mach_task_self(), &address,
            &size, &depth, reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
        if (status != KERN_SUCCESS) {
            result[@"vm_walk_result"] = @(status);
            break;
        }
        if (info.is_submap) {
            ++depth;
            continue;
        }
        const std::uint64_t dirty =
            static_cast<std::uint64_t>(info.pages_dirtied + info.pages_swapped_out) * vm_page_size;
        if (info.user_tag == 241) heapDirty += dirty;
        if (info.user_tag == 242) scratchDirty += dirty;
        if (info.user_wired_count != 0)
            wired += static_cast<std::uint64_t>(info.pages_resident) * vm_page_size;
        [regions addObject:@{
            @"address": @(address), @"size": @(size), @"tag": @(info.user_tag),
            @"protection": @(info.protection), @"max_protection": @(info.max_protection),
            @"resident_pages": @(info.pages_resident), @"dirty_pages": @(info.pages_dirtied),
            @"swapped_pages": @(info.pages_swapped_out), @"wired_count": @(info.user_wired_count),
            @"external_pager": @(info.external_pager), @"share_mode": @(info.share_mode),
        }];
        if (size == 0 || address + size <= address) break;
        address += size;
    }
    result[@"heap_dirty"] = @(heapDirty);
    result[@"scratch_dirty"] = @(scratchDirty);
    result[@"wired"] = @(wired);
    result[@"regions"] = regions;
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t threadCount = 0;
    const auto threadResult = task_threads(mach_task_self(), &threads, &threadCount);
    result[@"thread_info_result"] = @(threadResult);
    NSMutableArray *names = [NSMutableArray array];
    if (threadResult == KERN_SUCCESS) {
        for (mach_msg_type_number_t index = 0; index < threadCount; ++index) {
            thread_extended_info_data_t info {};
            count = THREAD_EXTENDED_INFO_COUNT;
            if (thread_info(threads[index], THREAD_EXTENDED_INFO,
                    reinterpret_cast<thread_info_t>(&info), &count) == KERN_SUCCESS)
                [names addObject:[NSString stringWithUTF8String:info.pth_name] ?: @""];
            mach_port_deallocate(mach_task_self(), threads[index]);
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
            threadCount * sizeof(thread_t));
    }
    result[@"threads"] = names;
    return result;
}
