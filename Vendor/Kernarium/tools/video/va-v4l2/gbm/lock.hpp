// SPDX-License-Identifier: GPL-2.0-or-later
// A mutex that needs no C++ runtime: this module is loaded into every process
// that uses gbm and carries none of its own.
#pragma once

#include <pthread.h>

namespace uml::gbm {

class Mutex {
public:
    void lock() noexcept { pthread_mutex_lock(&mutex_); }
    void unlock() noexcept { pthread_mutex_unlock(&mutex_); }

private:
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
};

class Guard {
public:
    explicit Guard(Mutex& mutex) noexcept : mutex_(mutex) { mutex_.lock(); }
    ~Guard() { mutex_.unlock(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    Mutex& mutex_;
};

}
