// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"

#include <cstdlib>
#include <string_view>

int main(int argc, char* argv[])
{
    if (argc == 2)
        uml::gpu::testing::runVhostUserServerChild(argv[1]);
    if (argc != 1)
        return EXIT_FAILURE;
    uml::gpu::testing::runVhostUserServerTests(argv[0]);
    uml::gpu::testing::runVhostUserProtocolTests();
    uml::gpu::testing::runInProcessServiceTests();
    std::puts("GPU lifecycle and ownership tests passed");
    return 0;
}
