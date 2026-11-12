// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "require.hpp"

#import <Metal/Metal.h>
#import <objc/runtime.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using uml::testing::require;
using uml::gpu::test::DrawContext;
using uml::gpu::test::SubmitStormConfig;

/* A virtualized GL or Vulkan stack ends every guest command buffer in a
 * submission, so the cost of one submission is the cost of its frames. On a
 * Metal backend that cost is dominated by the command storage an encoder
 * records into: a Metal command allocator owns that storage, creating one
 * makes the kernel allocate it, and releasing one gives it all back. A driver
 * that releases a one-time recording's allocators when the GPU finishes with
 * them therefore buys them again on the next submission, and both halves are
 * kernel calls (0.5 ms per submission on an M1 Ultra, against 0.05 ms when the
 * storage is recycled).
 *
 * The guard counts what that costs instead of timing it. The driver asks the
 * Metal device for an allocator with -newCommandAllocator, and hands a finished
 * recording's allocators back to its cache with -reset, the call Metal requires
 * once the GPU is done with the storage. Both are public Metal 4 selectors, so
 * the test counts them by replacing their implementations on the device and
 * allocator classes the driver uses. A time bound would fail whenever the host
 * is busy; a count depends only on whether the driver reuses what it was given. */

/* One recording in flight at a time: the allocators a completed recording
 * returns are the only ones the next recording can begin on, so reuse is the
 * only way to avoid creating them again. */
constexpr uint32_t kSerialSubmissions = 240;
/* A driver that does not recycle creates at least one allocator per
 * submission. The bound allows a quarter of the submissions to miss the cache,
 * which is far above what a driver that recycles misses (none, but for a thread
 * that is late handing storage back) and far below what one that does not
 * does. */
constexpr uint32_t kMaxCreationsPerSerialRound = kSerialSubmissions / 4;
/* More live recordings than the driver's cache can hold, so the path where it
 * runs out and creates storage runs in the same test. */
constexpr uint32_t kSubmissions = 240;
constexpr uint32_t kOutstanding = 32;

constexpr auto kCompletionDeadline = std::chrono::seconds(30);

std::atomic<uint32_t> allocatorsCreated{0};
std::atomic<uint32_t> allocatorsReset{0};
IMP createImplementation = nullptr;
IMP resetImplementation = nullptr;

/* The replacements keep the original's ownership convention by handling the
 * result as an untyped pointer: -newCommandAllocator returns an object the
 * caller owns, and ARC must not add a retain or release around it. */
void *countedNewCommandAllocator(id receiver, SEL command)
{
    allocatorsCreated.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<void *(*)(id, SEL)>(createImplementation)(receiver, command);
}

void countedReset(id receiver, SEL command)
{
    reinterpret_cast<void (*)(id, SEL)>(resetImplementation)(receiver, command);
    allocatorsReset.fetch_add(1, std::memory_order_relaxed);
}

/* Replaces the two selectors on the classes the default device and its
 * allocators belong to. The allocator class is learned from an allocator made
 * before the replacement, so that one is not counted. */
void count_command_storage()
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    require(device != nil, "no Metal device");
    id<MTL4CommandAllocator> sample = [device newCommandAllocator];
    require(sample != nil, "the device made no command allocator");

    Method create = class_getInstanceMethod(object_getClass(device),
                                            @selector(newCommandAllocator));
    Method reset = class_getInstanceMethod(object_getClass(sample), @selector(reset));
    require(create != nullptr && reset != nullptr,
            "the device or its allocator lacks a method the test counts");
    createImplementation = method_setImplementation(
        create, reinterpret_cast<IMP>(countedNewCommandAllocator));
    resetImplementation = method_setImplementation(
        reset, reinterpret_cast<IMP>(countedReset));
}

/* Blocks until the driver's completion handling has reset at least `target`
 * allocators since `base`. It is the point at which every completed recording
 * has handed its storage back, so a count taken after it does not depend on
 * which thread got there first. A driver that releases the storage instead of
 * handing it back never gets there. */
void await_resets(uint32_t base, uint32_t target)
{
    const auto deadline = std::chrono::steady_clock::now() + kCompletionDeadline;
    while (allocatorsReset.load(std::memory_order_acquire) - base < target) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(
                "command storage is not handed back on completion: " +
                std::to_string(allocatorsReset.load() - base) + " of " +
                std::to_string(target) + " allocators reset after " +
                std::to_string(kCompletionDeadline.count()) + " s");
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

void require_values(const uml::gpu::test::SubmitStormResult &result, uint32_t submissions,
                    const char *what)
{
    require(result.values.size() == submissions,
            std::string(what) + " returned the wrong number of slots");
    for (uint32_t index = 0; index < submissions; ++index) {
        require(result.values[index] == index + 1u,
                std::string(what) + ": submission " + std::to_string(index) + " wrote " +
                    std::to_string(result.values[index]) + ", expected " +
                    std::to_string(index + 1u));
    }
}

} // namespace

int main()
{
    try {
        count_command_storage();

        /* One recording at a time, each waited for and then given time to hand
         * its storage back before the next begins. The first recording finds
         * the cache empty and creates every allocator it records into; after
         * that those are the whole working set, so a driver that recycles
         * creates (almost) nothing more. */
        {
            DrawContext context;
            const uint32_t createdBefore = allocatorsCreated.load();
            const uint32_t resetBefore = allocatorsReset.load();
            uint32_t completions = 0;
            uint32_t perRecording = 0;
            uint32_t createdAfterFirst = 0;

            SubmitStormConfig serial;
            serial.submissions = kSerialSubmissions;
            serial.outstanding = 1;
            serial.completed = [&](uint32_t) {
                if (++completions == 1) {
                    createdAfterFirst = allocatorsCreated.load();
                    perRecording = createdAfterFirst - createdBefore;
                    require(perRecording != 0,
                            "the counter saw no command storage created for a recording");
                }
                await_resets(resetBefore, completions * perRecording);
            };
            const auto result = context.submit_storm(serial);
            require_values(result, kSerialSubmissions, "serial storm");

            const uint32_t created = allocatorsCreated.load() - createdAfterFirst;
            std::printf("command storage: %u serial submissions of %u allocators each made %u more "
                        "(bound %u)\n", kSerialSubmissions, perRecording, created,
                        kMaxCreationsPerSerialRound);
            std::fflush(stdout);
            require(created <= kMaxCreationsPerSerialRound,
                    std::to_string(created) + " allocators created over " +
                        std::to_string(kSerialSubmissions - 1) + " serial submissions, over the " +
                        std::to_string(kMaxCreationsPerSerialRound) +
                        " bound: command storage is not being reused");
        }
        std::puts("PASS command storage: serial submissions reuse the storage a completion hands back");

        /* More recordings in flight than the cache holds: the driver has to
         * create storage when the cache runs dry and drop what the cache cannot
         * keep, and every recording must still land where it belongs. */
        {
            DrawContext context;
            const uint32_t resetBefore = allocatorsReset.load();
            uint32_t completions = 0;

            SubmitStormConfig storm;
            storm.submissions = kSubmissions;
            storm.outstanding = kOutstanding;
            storm.completed = [&](uint32_t) {
                ++completions;
                await_resets(resetBefore, completions);
            };
            const auto result = context.submit_storm(storm);
            require_values(result, kSubmissions, "submit storm");
            require(completions == kSubmissions, "the storm reported " +
                        std::to_string(completions) + " completions for " +
                        std::to_string(kSubmissions) + " submissions");
        }
        std::puts("PASS command storage: recordings beyond the cache each execute in place");

        /* Destroying the device with submissions still in flight: the backend
         * may only complete them through state that outlives the device, and
         * anything it recycles has to survive that. */
        {
            DrawContext context;
            SubmitStormConfig abandoned;
            abandoned.submissions = kOutstanding * 2;
            abandoned.outstanding = kOutstanding;
            abandoned.abandon = true;
            const auto result = context.submit_storm(abandoned);
            require(result.submissions == kOutstanding * 2,
                    "abandoned storm reported the wrong submission count");
        }
        std::puts("PASS command storage: device destroyed under outstanding submissions");

        /* A device created after that teardown must still record and execute,
         * which is what a recycled allocator handed back by a late completion
         * would break. */
        {
            DrawContext context;
            SubmitStormConfig again;
            again.submissions = kOutstanding;
            again.outstanding = kOutstanding;
            const auto result = context.submit_storm(again);
            for (uint32_t index = 0; index < kOutstanding; ++index) {
                require(result.values[index] == index + 1u,
                        "post-teardown submission " + std::to_string(index) +
                            " did not execute");
            }
        }
        std::puts("PASS command storage: recording after a teardown still executes");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL command storage: %s\n", error.what());
        return 1;
    }
}
