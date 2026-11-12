// SPDX-License-Identifier: GPL-2.0-or-later

/* Every GPU unit test process keeps its own Metal shader cache.
 *
 * A process without a bundle identifier, such as a unit test, shares
 * $(getconf DARWIN_USER_CACHE_DIR)/com.apple.metal with every other such
 * process of the user. Opening that cache costs a read of its whole index,
 * which a long-lived host leaves at tens of megabytes, and the first pipeline
 * a process compiles pays for it. Plain, that is half a second. Under
 * AddressSanitizer every realloc copies, and the same open took three minutes
 * of a test's first Metal command, so a test timed out or ran for minutes
 * depending on what else had run on the host. A cache of the test's own is
 * empty at the start, so what a test costs no longer depends on the host's
 * history, and it leaves the host's cache as it found it.
 *
 * Metal's public API has no call to place the cache. Its binary exports
 * MTLSetShaderCachePath, which takes an NSString for the directory, and the
 * directory must exist already or Metal does not cache at all. It applies to
 * the first use only. The function is not declared in a header, so this file
 * looks it up: a process that does not load Metal, or a Metal without the
 * function, keeps the shared cache and only runs slower. The constructor runs
 * before main() and before any other initializer of the test, so nothing has
 * used the cache yet. */

#include "scratch_directory.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <dlfcn.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr std::string_view cachePrefix = "metal-shader-cache-";

std::filesystem::path& cacheDirectory()
{
    static std::filesystem::path directory;
    return directory;
}

void removeCache()
{
    std::error_code ignored;
    std::filesystem::remove_all(cacheDirectory(), ignored);
}

/* A child the test starts again and ends with _Exit never runs this
 * process's exit handler, and a child the test kills cannot. The caches of
 * processes that are gone are removed by whichever test process exits next,
 * so the scratch directory ends empty. A directory whose process is running,
 * such as a second copy of the test, is left alone. */
void removeCachesOfExitedProcesses()
{
    std::vector<std::filesystem::path> exited;
    std::error_code error;
    std::filesystem::directory_iterator entry(
        cacheDirectory().parent_path(), error);
    for (; !error && entry != std::filesystem::directory_iterator {};
         entry.increment(error)) {
        const std::string name = entry->path().filename().string();
        if (!name.starts_with(cachePrefix))
            continue;
        const char* const first = name.data() + cachePrefix.size();
        const char* const last = name.data() + name.size();
        int process = 0;
        const auto parsed = std::from_chars(first, last, process);
        if (parsed.ec == std::errc {} && parsed.ptr == last && process > 0 &&
            kill(process, 0) != 0 && errno == ESRCH) {
            exited.push_back(entry->path());
        }
    }
    for (const std::filesystem::path& directory : exited) {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
}

void removeCacheAtExit()
{
    removeCache();
    removeCachesOfExitedProcesses();
}

[[gnu::constructor(101)]] void isolateMetalShaderCache()
{
    using SetShaderCachePath = void (*)(CFStringRef);
    void* const symbol = dlsym(RTLD_DEFAULT, "MTLSetShaderCachePath");
    if (symbol == nullptr) {
        if (dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice") != nullptr) {
            std::fputs("metal shader cache isolation: Metal has no "
                       "MTLSetShaderCachePath; this process shares the "
                       "host's shader cache\n",
                stderr);
        }
        return;
    }

    /* One directory per process: a test that runs the same binary twice at
     * once must not remove the other's cache when it exits. */
    cacheDirectory() = uml::testing::scratch_directory() /
        (std::string {cachePrefix} + std::to_string(getpid()));
    removeCache();
    std::error_code error;
    std::filesystem::create_directories(cacheDirectory(), error);
    if (error) {
        std::fprintf(stderr, "metal shader cache isolation: cannot create %s: %s\n",
            cacheDirectory().c_str(), error.message().c_str());
        std::abort();
    }
    std::atexit(removeCacheAtExit);

    const std::string path = cacheDirectory().string();
    CFStringRef const name = CFStringCreateWithCString(
        kCFAllocatorDefault, path.c_str(), kCFStringEncodingUTF8);
    if (name == nullptr) {
        std::fprintf(stderr, "metal shader cache isolation: %s is not UTF-8\n",
            path.c_str());
        std::abort();
    }
    reinterpret_cast<SetShaderCachePath>(symbol)(name);
    CFRelease(name);
}

} // namespace
