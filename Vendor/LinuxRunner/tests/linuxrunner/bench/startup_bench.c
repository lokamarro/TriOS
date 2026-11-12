#define _GNU_SOURCE
// Startup-path microbenchmarks inside the guest: per-operation cost of the
// syscalls and process operations that dominate application launch (path
// lookups, opens, small reads, mappings, process creation, exec and dynamic
// linking). Prints one "BENCH name=... ns_per_op=... ops=..." line each, plus
// the median of 5 repetitions. Run the same binary natively for a baseline.
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t) t.tv_sec * 1000000000ull + t.tv_nsec; }
static int cmp(const void *a, const void *b) { double x = *(const double *) a, y = *(const double *) b; return x < y ? -1 : x > y; }

typedef void (*bench_fn)(int n);
static const char *stat_paths[] = {"/usr/lib/aarch64-linux-gnu/libc.so.6", "/etc/passwd", "/usr/share/fonts", "/usr/bin/sh", "/usr/lib/aarch64-linux-gnu/libgtk-3.so.0", "/nonexistent/file"};

static void b_getppid(int n) { for (int i = 0; i < n; i++) syscall(SYS_getppid); }
static void b_stat(int n) { struct stat st; for (int i = 0; i < n; i++) stat(stat_paths[i % 6], &st); }
static void b_open_close(int n) { for (int i = 0; i < n; i++) { int fd = open("/etc/passwd", O_RDONLY | O_CLOEXEC); if (fd >= 0) close(fd); } }
static void b_read4k(int n) { char buf[4096]; int fd = open("/etc/passwd", O_RDONLY); for (int i = 0; i < n; i++) pread(fd, buf, sizeof buf, 0); close(fd); }
static void b_mmap(int n) { int fd = open("/usr/lib/aarch64-linux-gnu/libc.so.6", O_RDONLY); for (int i = 0; i < n; i++) { void *p = mmap(NULL, 65536, PROT_READ, MAP_PRIVATE, fd, 0); if (p != MAP_FAILED) munmap(p, 65536); } close(fd); }
static void b_pipe_rt(int n) {
    int a[2], b[2]; if (pipe(a) || pipe(b)) return;
    pid_t c = fork();
    if (c == 0) { close(a[1]); close(b[0]); char x; while (read(a[0], &x, 1) == 1) if (write(b[1], &x, 1) != 1) break; _exit(0); }
    close(a[0]); close(b[1]);   // the echo child must see EOF when we close a[1]
    char x = 'x';
    for (int i = 0; i < n; i++) { if (write(a[1], &x, 1) != 1 || read(b[0], &x, 1) != 1) break; }
    close(a[1]); waitpid(c, NULL, 0); close(b[0]);
}
static void b_fork(int n) { for (int i = 0; i < n; i++) { pid_t c = fork(); if (c == 0) _exit(0); waitpid(c, NULL, 0); } }
static void b_exec_true(int n) { for (int i = 0; i < n; i++) { pid_t c = fork(); if (c == 0) { execl("/bin/true", "true", (char *) NULL); _exit(127); } waitpid(c, NULL, 0); } }
static void b_readdir(int n) { for (int i = 0; i < n; i++) { DIR *d = opendir("/usr/lib/aarch64-linux-gnu"); if (!d) return; while (readdir(d)) {} closedir(d); } }

static void run(const char *name, bench_fn fn, int n) {
    double samples[5];
    fn(n / 10 + 1);   // warm-up
    for (int r = 0; r < 5; r++) { uint64_t t0 = now_ns(); fn(n); samples[r] = (double) (now_ns() - t0) / n; }
    qsort(samples, 5, sizeof samples[0], cmp);
    printf("BENCH name=%s ns_per_op=%.0f min=%.0f max=%.0f ops=%d\n", name, samples[2], samples[0], samples[4], n);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int scale = argc > 1 ? atoi(argv[1]) : 1;
    run("getppid", b_getppid, 20000 * scale);
    run("stat_mixed", b_stat, 6000 * scale);
    run("open_close", b_open_close, 3000 * scale);
    run("pread_4k", b_read4k, 5000 * scale);
    run("mmap_munmap_64k", b_mmap, 2000 * scale);
    run("pipe_roundtrip", b_pipe_rt, 2000 * scale);
    run("readdir_libdir", b_readdir, 20 * scale);
    run("fork_wait", b_fork, 60 * scale);
    run("fork_exec_true", b_exec_true, 30 * scale);
    return 0;
}
