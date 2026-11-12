#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <unistd.h>

// LinuxRunner accounting regression: glibc-style guarded arenas must consume
// backing only when touched, not when mprotect enables their reserved range.
#define ARENAS 40
#define SIZE (128UL * 1024 * 1024)
static int denied_access(volatile unsigned char *p, int write) {
    pid_t child = fork();
    if (child < 0) return 0;
    if (!child) { if (write) *p = 1; else (void)*p; _exit(99); }
    int status;
    return waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
}
int main(void) {
    volatile unsigned char *p[ARENAS];
    struct sysinfo before, reserved, touched, after;
    if (sysinfo(&before)) return 1;
    for (int i = 0; i < ARENAS; i++) {
        p[i] = mmap((void *)((uintptr_t)(i + 1) << 40), SIZE + 65536, PROT_NONE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p[i] == MAP_FAILED) return 2;
        if (mprotect((void *)(p[i] + 65536), SIZE, PROT_READ | PROT_WRITE)) return 3;
    }
    if (sysinfo(&reserved)) return 4;
    if (before.freeram - reserved.freeram > 65536) {
        fprintf(stderr, "untouched arenas eagerly charged: %lu bytes\n",
            before.freeram - reserved.freeram);
        return 5;
    }
    if (!denied_access(p[0], 0)) return 16;
    for (int i = 0; i < ARENAS; i++) {
        if (p[i][65536] || p[i][65536 + SIZE - 1]) return 6;
        p[i][65536] = 17; p[i][65536 + SIZE - 1] = 29;
    }
    if (sysinfo(&touched) || reserved.freeram - touched.freeram != ARENAS * 8192UL) return 7;
    pid_t child = fork();
    if (child < 0) return 8;
    if (!child) {
        for (int i = 0; i < ARENAS; i++) {
            if (p[i][65536] != 17 || p[i][65536 + SIZE/2]) _exit(9);
            p[i][65536] = 31; p[i][65536 + SIZE/2] = 47;
        }
        _exit(0);
    }
    int status;
    if (waitpid(child, &status, 0) != child || status) return 10;
    for (int i = 0; i < ARENAS; i++) {
        if (p[i][65536] != 17 || p[i][65536 + SIZE/2]) return 11;
        if (mprotect((void *)(p[i] + 65536), SIZE, PROT_READ)) return 12;
        if (i == 0 && !denied_access(p[i] + 65536, 1)) return 17;
        if (mprotect((void *)(p[i] + 65536), SIZE, PROT_READ | PROT_WRITE)) return 13;
        p[i][65536] = 53;
        if (munmap((void *)p[i], SIZE + 65536)) return 14;
    }
    if (sysinfo(&after) || after.freeram != before.freeram) return 15;
    puts("PROTNONE_ARENA_PASS 5 GiB reserved, demand-zero charge, fork CoW, guard/read-only faults, protection transitions, unmap accounting");
    return 0;
}
