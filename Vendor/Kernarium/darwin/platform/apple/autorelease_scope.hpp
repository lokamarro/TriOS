// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

extern "C" {
void* objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void* pool);
}

namespace uml::platform::apple {

/*
 * Bounds an Objective-C autorelease pool over a scope, the same pair of calls
 * @autoreleasepool compiles to, so a plain C++ translation unit can drain what
 * a framework autoreleases on this thread without becoming Objective-C++.
 *
 * Metal and its IOGPU layer autorelease per submission. A worker thread the
 * emulator created has no run loop and therefore no pool of its own, so
 * without a scope like this those objects accumulate for the lifetime of the
 * thread rather than for the lifetime of the work.
 */
class AutoreleaseScope {
public:
    AutoreleaseScope() noexcept
        : pool_(objc_autoreleasePoolPush())
    {
    }

    ~AutoreleaseScope()
    {
        objc_autoreleasePoolPop(pool_);
    }

    AutoreleaseScope(const AutoreleaseScope&) = delete;
    AutoreleaseScope& operator=(const AutoreleaseScope&) = delete;

private:
    void* pool_;
};

} // namespace uml::platform::apple
