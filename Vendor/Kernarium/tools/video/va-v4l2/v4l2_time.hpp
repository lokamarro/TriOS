// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <sys/time.h>

#include <cstdint>

namespace uml::va {

// V4L2 carries a buffer timestamp as a timeval, so a key that has to survive
// the round trip through both queues is a whole number of microseconds.
inline std::uint64_t toNanoseconds(const timeval& time) noexcept
{
    return std::uint64_t(time.tv_sec) * 1000000000ull + std::uint64_t(time.tv_usec) * 1000ull;
}

inline timeval fromNanoseconds(std::uint64_t value) noexcept
{
    timeval time {};
    time.tv_sec = time_t(value / 1000000000ull);
    time.tv_usec = suseconds_t(value % 1000000000ull / 1000ull);
    return time;
}

}
