// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"
#include "in_process_service.hpp"
#include "vhost_user_message_testing.hpp"

#include <chrono>
#include <cstring>
#include <sys/wait.h>

namespace uml::gpu::testing {
namespace {

[[noreturn]] void runPartialFrameChild(bool partialPayload)
{
    resetStopWaitObserver();
    InProcessGpuOptions options;
    options.vncPort = 0;
    options.width = 64;
    options.height = 64;
    InProcessGpuService service {std::move(options)};
    const int transport = descriptorFromName(service.takeConnectionName());
    writePartialFrame(transport, partialPayload);

    require(waitForStopWaitCount(1, 1000));
    service.stop();
    require(::close(transport) == 0);
    std::_Exit(EXIT_SUCCESS);
}

void partialFrameUsesWake(const char* executable, bool partialPayload)
{
    const auto before = descriptorCount();
    const pid_t child = launchChild(
        executable,
        partialPayload ? "--partial-payload-child" : "--partial-header-child");

    // The linked test-only transport polls every five seconds. The child can
    // finish inside this two-second watchdog only if InProcessGpuService::stop
    // interrupts the blocked endpoint through its separately owned duplicate.
    const int status = waitForChild(child, std::chrono::seconds {2});
    require(WIFEXITED(status));
    require(WEXITSTATUS(status) == EXIT_SUCCESS);
    require(descriptorCount() == before);
}

} // namespace
} // namespace uml::gpu::testing

int main(int argc, char* argv[])
{
    using namespace uml::gpu::testing;
    if (argc == 2) {
        if (std::strcmp(argv[1], "--partial-header-child") == 0)
            runPartialFrameChild(false);
        if (std::strcmp(argv[1], "--partial-payload-child") == 0)
            runPartialFrameChild(true);
        return EXIT_FAILURE;
    }
    if (argc != 1)
        return EXIT_FAILURE;
    partialFrameUsesWake(argv[0], false);
    partialFrameUsesWake(argv[0], true);
    std::puts("GPU in-process stop wake tests passed");
    return 0;
}
