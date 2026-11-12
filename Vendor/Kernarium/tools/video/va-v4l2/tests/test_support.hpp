// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdio>

namespace uml::va::test {

inline int& failureCount() noexcept
{
    static int count = 0;
    return count;
}

inline void check(bool condition, const char* what)
{
    if (condition) return;
    std::fprintf(stderr, "FAIL %s\n", what);
    ++failureCount();
}

void runPoolTests();
void runEncoderTests();
void runDriverEncodeTests();
void runAnnexbTests();
void runGbmPlanarTests();

}
