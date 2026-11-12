// Memory charged for threads and malloc arenas (found on Arch Linux ARM: GTK
// processes with glycin threads showed several writable 128 MiB regions, the
// desktop exhausted LinuxRunner's anonymous-page budget, and execve then failed
// with ENOMEM -> SIGSEGV).
//   1. RLIMIT_STACK defaults to Linux's 8 MiB soft limit (glibc sizes every
//      default thread stack from it) and can be raised (hard limit unlimited).
//   2. 16 idle threads with default stacks charge far less than 16 x 128 MiB.
//   3. glibc new_heap(): reserve 2 x 64 MiB PROT_NONE, trim to an aligned 64 MiB,
//      mprotect a small head writable: only touched pages are charged; unmapping
//      returns the charge.
// The charge is read from MemFree, which LinuxRunner derives from the budget.
// Prints ARENA_RESERVATION_CONTRACT PASS|FAIL.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#define HEAP_MAX (64UL << 20)
#define HEAD (132UL << 10)
#define COUNT 16
static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL " __VA_ARGS__); printf(" (errno=%d)\n", errno); } } while (0)

static long mem_free_kb(void) {
    FILE *f = fopen("/proc/meminfo", "r"); char line[256]; long kb = -1;
    while (f && fgets(line, sizeof line, f)) if (sscanf(line, "MemFree: %ld kB", &kb) == 1) break;
    if (f) fclose(f);
    return kb;
}

static char *new_heap(void) {
    char *p1 = mmap(NULL, HEAP_MAX << 1, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p1 == MAP_FAILED) return NULL;
    char *p2 = (char *)(((uintptr_t)p1 + (HEAP_MAX - 1)) & ~(HEAP_MAX - 1));
    size_t ul = p2 - p1;
    if (ul) munmap(p1, ul);
    munmap(p2 + HEAP_MAX, HEAP_MAX - ul);
    if (mprotect(p2, HEAD, PROT_READ | PROT_WRITE) != 0) return NULL;
    memset(p2, 1, 4096);
    return p2;
}

static pthread_barrier_t started, finish;
static void *idle(void *arg) { (void)arg; pthread_barrier_wait(&started); pthread_barrier_wait(&finish); return NULL; }

int main(void) {
    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur == 8UL << 20 && rl.rlim_max == RLIM_INFINITY,
          "RLIMIT_STACK default 8 MiB soft / unlimited hard (got %lu / %lu)", (unsigned long)rl.rlim_cur, (unsigned long)rl.rlim_max);

    long before = mem_free_kb();
    pthread_t threads[COUNT];
    pthread_barrier_init(&started, NULL, COUNT + 1); pthread_barrier_init(&finish, NULL, COUNT + 1);
    int created = 0;
    for (int i = 0; i < COUNT; i++) if (pthread_create(&threads[i], NULL, idle, NULL) == 0) created++;
    CHECK(created == COUNT, "created %d threads", created);
    if (created == COUNT) pthread_barrier_wait(&started);
    long with_threads = mem_free_kb();
    long thread_mib = (before - with_threads) / 1024;
    CHECK(before > 0 && thread_mib < 256, "%d idle threads charged %ld MiB", COUNT, thread_mib);
    if (created == COUNT) { pthread_barrier_wait(&finish); for (int i = 0; i < COUNT; i++) pthread_join(threads[i], NULL); }

    before = mem_free_kb();
    char *heaps[COUNT];
    for (int i = 0; i < COUNT; i++) { heaps[i] = new_heap(); CHECK(heaps[i] != NULL, "arena %d", i); }
    long after = mem_free_kb();
    long arena_mib = (before - after) / 1024;
    CHECK(before > 0 && after > 0 && arena_mib < 64, "%d arenas charged %ld MiB", COUNT, arena_mib);
    if (heaps[0]) {
        CHECK(mprotect(heaps[0] + HEAD, 1 << 20, PROT_READ | PROT_WRITE) == 0, "grow by mprotect");
        heaps[0][HEAD + 4096] = 2;
    }
    for (int i = 0; i < COUNT; i++) if (heaps[i]) munmap(heaps[i], HEAP_MAX);
    long released = mem_free_kb();
    CHECK(released >= before - 1024, "unmapping returns the charge (MemFree %ld -> %ld kB)", before, released);
    printf("ARENA_RESERVATION_CONTRACT %s failures=%d threads_mib=%ld arenas_mib=%ld\n", failures ? "FAIL" : "PASS", failures, thread_mib, arena_mib);
    return failures != 0;
}
