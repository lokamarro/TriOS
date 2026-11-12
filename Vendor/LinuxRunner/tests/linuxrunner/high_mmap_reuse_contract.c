#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
    const size_t arena = 256UL * 1024 * 1024;
    uintptr_t highest = 0;
    for (unsigned i = 0; i < 600; i++) {
        unsigned char *p = mmap(NULL, arena, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) {
            fprintf(stderr, "HIGH_MMAP_REUSE_FAIL iteration=%u errno=%d highest=%#lx\n", i, errno, (unsigned long)highest);
            return 1;
        }
        if ((uintptr_t)p > highest) highest = (uintptr_t)p;
        assert(!mprotect(p, 4096, PROT_READ | PROT_WRITE));
        p[0] = (unsigned char)i;
        assert(p[0] == (unsigned char)i);
        assert(!munmap(p, arena));
    }
    // Live reservations and their materialized pages must never be overwritten.
    unsigned char *live[12];
    for (unsigned i = 0; i < 12; i++) {
        live[i] = mmap(NULL, arena, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        assert(live[i] != MAP_FAILED && !mprotect(live[i], 4096, PROT_READ | PROT_WRITE));
        live[i][0] = 40 + i;
        for (unsigned j = 0; j < i; j++) assert((uintptr_t)live[i] + arena <= (uintptr_t)live[j] || (uintptr_t)live[j] + arena <= (uintptr_t)live[i]);
    }
    for (unsigned i = 0; i < 12; i += 2) assert(!munmap(live[i], arena));
    unsigned char *large = mmap(NULL, 2 * arena, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    assert(large != MAP_FAILED && !mprotect(large, 4096, PROT_READ | PROT_WRITE));
    large[0] = 99;
    for (unsigned i = 1; i < 12; i += 2) { assert(live[i][0] == 40 + i); assert(!munmap(live[i], arena)); }
    assert(!munmap(large, 2 * arena));
    printf("HIGH_MMAP_REUSE_PASS cycles=600 live_reservations=12 highest=%#lx\n", (unsigned long)highest);
    return 0;
}
