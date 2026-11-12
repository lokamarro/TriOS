#ifndef UTIL_SYNC_H
#define UTIL_SYNC_H

#include <stdatomic.h>
#include <sched.h>
#include <pthread.h>
#include <stdbool.h>
#include <assert.h>
#include <setjmp.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include "misc.h"
#include "debug.h"

// locks, implemented using pthread

#define LOCK_DEBUG 0

typedef struct {
    pthread_mutex_t m;
    pthread_t owner;
#if LOCK_DEBUG
    struct lock_debug {
        const char *file; // doubles as locked
        int line;
        int pid;
        bool initialized;
    } debug;
#endif
} lock_t;

static inline void lock_init(lock_t *lock) {
    pthread_mutex_init(&lock->m, NULL);
#if LOCK_DEBUG
    lock->debug = (struct lock_debug) {
        .initialized = true,
    };
#endif
}

#if LOCK_DEBUG
#define LOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, 0, { .initialized = true }}
#else
#define LOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, 0}
#endif
static inline void __lock(lock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    pthread_mutex_lock(&lock->m);
    lock->owner = pthread_self();
#if LOCK_DEBUG
    assert(lock->debug.initialized);
    assert(!lock->debug.file && "Attempting to recursively lock");
    lock->debug.file = file;
    lock->debug.line = line;
    extern int current_pid(void);
    lock->debug.pid = current_pid();
#endif
}
#define lock(lock) __lock(lock, __FILE__, __LINE__)
static inline void unlock(lock_t *lock) {
#if LOCK_DEBUG
    assert(lock->debug.initialized);
    assert(lock->debug.file && "Attempting to unlock an unlocked lock");
    lock->debug = (struct lock_debug) { .initialized = true };
#endif
    lock->owner = zero_init(pthread_t);
    pthread_mutex_unlock(&lock->m);
}

static inline int trylock(lock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    int status = pthread_mutex_trylock(&lock->m);
#if LOCK_DEBUG
    if (!status) {
        lock->debug.file = file;
        lock->debug.line = line;
        extern int current_pid(void);
        lock->debug.pid = current_pid();
    }
#endif
    return status;
}
#define trylock(lock) trylock(lock, __FILE__, __LINE__)

// conditions, implemented using pthread conditions but hacked so you can also
// be woken by a signal

typedef struct {
    pthread_cond_t cond;
} cond_t;
#define COND_INITIALIZER ((cond_t) {PTHREAD_COND_INITIALIZER})

// Must call before using the condition
void cond_init(cond_t *cond);
// Must call when finished with the condition (currently doesn't do much but might do something important eventually I guess)
void cond_destroy(cond_t *cond);
// Releases the lock, waits for the condition, and reacquires the lock.
// Returns _EINTR if waiting stopped because the thread received a signal,
// _ETIMEDOUT if waiting stopped because the timout expired, 0 otherwise.
// Will never return _ETIMEDOUT if timeout is NULL.
int must_check wait_for(cond_t *cond, lock_t *lock, struct timespec *timeout);
// Same as wait_for, except it will never return _EINTR
int wait_for_ignore_signals(cond_t *cond, lock_t *lock, struct timespec *timeout);
// Wake up all waiters.
void notify(cond_t *cond);
// Wake up one waiter.
void notify_once(cond_t *cond);

// this is a read-write lock that prefers writers, i.e. if there are any
// writers waiting a read lock will block.
// on darwin pthread_rwlock_t is already like this, on linux you can configure
// it to prefer writers. not worrying about anything else right now.
typedef struct {
    pthread_rwlock_t l;
    // 0: unlocked
    // -1: write-locked
    // >0: read-locked with this many readers
    atomic_int val;
    // Writers retrying in __write_wrlock (see read_wrlock_yield_to_writers).
    atomic_int writers_waiting;
    const char *file;
    int line;
    int pid;
} wrlock_t;
static inline void wrlock_init(wrlock_t *lock) {
    pthread_rwlockattr_t *pattr = NULL;
#if defined(__GLIBC__)
    pthread_rwlockattr_t attr;
    pattr = &attr;
    pthread_rwlockattr_init(pattr);
    pthread_rwlockattr_setkind_np(pattr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
    if (pthread_rwlock_init(&lock->l, pattr)) __builtin_trap();
    lock->val = lock->line = lock->pid = 0;
    atomic_store(&lock->writers_waiting, 0);
    lock->file = NULL;
}

extern int current_pid(void);
static inline void wrlock_destroy(wrlock_t *lock) {
    if (pthread_rwlock_destroy(&lock->l) != 0) __builtin_trap();
}
static inline void read_wrlock(wrlock_t *lock) {
    if (pthread_rwlock_rdlock(&lock->l) != 0) __builtin_trap();
    assert(lock->val >= 0);
    lock->val++;
}
// Interpreter quantum boundary. A writer (mmap, mprotect, munmap, CoW) retries
// trywrlock with sleeps; a guest thread that spins (busy-wait loop) releases
// the read lock and re-takes it within nanoseconds, so without this the writer
// never gets in and e.g. pthread_create's mprotect hangs for as long as any
// other thread spins. Let a waiting writer in, but only for a bounded time
// (one ~20 us nap per quantum, while a writer waits) so neither a writer that
// itself waits for this thread nor a stream of writers can stall readers.
static inline void read_wrlock_yield_to_writers(wrlock_t *lock) {
    // Yield the CPU (microseconds, unlike nanosleep's coarse host timer) until
    // the waiting writer got in, for at most 64 yields.
    for (int i = 0; i < 64 && atomic_load_explicit(&lock->writers_waiting, memory_order_relaxed) > 0; i++)
        sched_yield();
    read_wrlock(lock);
}
static inline bool read_wrtrylock(wrlock_t *lock) {
    int err = pthread_rwlock_tryrdlock(&lock->l);
    if (err == EBUSY || err == EAGAIN) return false;
    if (err != 0) __builtin_trap();
    assert(lock->val >= 0);
    lock->val++;
    return true;
}
static inline void read_wrunlock(wrlock_t *lock) {
    assert(lock->val > 0);
    lock->val--;
    if (pthread_rwlock_unlock(&lock->l) != 0) __builtin_trap();
}
// Called on each retry while a writer waits in __write_wrlock. The kernel
// pokes the other threads executing guest code under that lock: a thread
// looping in chained code otherwise never reaches its quantum boundary, never
// drops its read lock, and the writer (mmap, mprotect, munmap) waits forever.
extern void (*wrlock_writer_waiting_hook)(wrlock_t *lock);
static inline void __write_wrlock(wrlock_t *lock, const char *file, int line) {
    // Acquire the write lock without letting a *waiting* writer freeze out
    // readers (which macOS's writer-preferring pthread_rwlock does). We spin
    // on trywrlock instead of a blocking wrlock: while we're merely retrying
    // (not queued as a waiter), concurrent readers can still take the lock.
    //
    // This is load-bearing for the JIT: task_run_current holds mem->lock as a
    // READER for the duration of each JIT quantum and re-acquires it every
    // ~1024 blocks. Under a blocking writer-preferring wrlock, one thread
    // requesting the write lock (mmap/munmap/CoW, e.g. JSC's GC sweep) freezes
    // EVERY other JIT thread at its next read_wrlock — and if the writer is
    // itself waiting on those threads to reach a GC safepoint, the whole VM
    // deadlocks (observed intermittently in claude-cli startup). Spinning
    // keeps readers live so they can progress and release, letting the writer
    // eventually win a trylock. Back off from 1us to a steady 100us sleep;
    // never fall back to blocking pthread_rwlock_wrlock().
    long pause_ns = 1000;
    if (pthread_rwlock_trywrlock(&lock->l) != 0) {
        // Announce the wait and poke readers out of guest code (rate-limited).
        // Retry every ~1-2 us for the first ~2 ms: a poked reader naps briefly
        // at its quantum boundary (read_wrlock_yield_to_writers) and the
        // writer must land in that gap. Then back off as before.
        atomic_fetch_add(&lock->writers_waiting, 1);
        unsigned tries = 0;
        while (pthread_rwlock_trywrlock(&lock->l) != 0) {
            if (wrlock_writer_waiting_hook && (tries & 63) == 0)
                wrlock_writer_waiting_hook(lock);
            if (tries < 1024) {
                sched_yield();
            } else {
                const struct timespec pause = { .tv_sec = 0, .tv_nsec = pause_ns };
                nanosleep(&pause, NULL);
                if (pause_ns < 100000)
                    pause_ns *= 2;
            }
            tries++;
        }
        atomic_fetch_sub(&lock->writers_waiting, 1);
    }
    assert(lock->val == 0);
    lock->val = -1;
    lock->file = file;
    lock->line = line;
    lock->pid = current_pid();
}
#define write_wrlock(lock) __write_wrlock(lock, __FILE__, __LINE__)
static inline bool write_wrtrylock(wrlock_t *lock) {
    int err = pthread_rwlock_trywrlock(&lock->l);
    if (err == EBUSY) return false;
    if (err != 0) __builtin_trap();
    assert(lock->val == 0);
    lock->val = -1;
    return true;
}
static inline void write_wrunlock(wrlock_t *lock) {
    assert(lock->val == -1);
    lock->val = lock->line = lock->pid = 0;
    lock->file = NULL;
    if (pthread_rwlock_unlock(&lock->l) != 0) __builtin_trap();
}

extern __thread sigjmp_buf unwind_buf;
extern __thread bool should_unwind;
static inline int sigunwind_start(void) {
    if (sigsetjmp(unwind_buf, 1)) {
        should_unwind = false;
        return 1;
    } else {
        should_unwind = true;
        return 0;
    }
}
static inline void sigunwind_end(void) {
    should_unwind = false;
}

#endif
