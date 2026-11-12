/* Process CPU time covers every thread (Linux): getrusage(RUSAGE_SELF),
 * clock_gettime(CLOCK_PROCESS_CPUTIME_ID), times() and /proc/<pid>/stat
 * utime+stime, as ps/top/htop and profilers read them. RUSAGE_THREAD is the
 * calling thread only; times() children fields are reaped children only. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static atomic_int stop;
static int fails;
#define STEP(name) do { fprintf(stderr, "STEP %s\n", name); fflush(stderr); } while (0)
static void check(const char *what, int ok, double value) { printf("%s %s (%.2f)\n", ok ? "PASS" : "FAIL", what, value); if (!ok) fails++; }
static double now(clockid_t c) { struct timespec t; clock_gettime(c, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void *spin(void *arg) { (void)arg; volatile unsigned long x = 0; while (!atomic_load(&stop)) x++; return NULL; }
static double tv(struct timeval t) { return t.tv_sec + t.tv_usec / 1e6; }
static double proc_stat_seconds(void) {
    FILE *f = fopen("/proc/self/stat", "r"); char line[1024]; unsigned long u = 0, s = 0;
    if (!f || !fgets(line, sizeof line, f)) return -1;
    fclose(f);
    char *p = line; for (int field = 1; field < 14 && p; field++) { p = strchr(p, ' '); if (p) p++; }
    if (!p || sscanf(p, "%lu %lu", &u, &s) != 2) return -1;
    return (u + s) / (double)sysconf(_SC_CLK_TCK);
}
int main(void) {
    pthread_t threads[3];
    STEP("start");
    double wall0 = now(CLOCK_MONOTONIC);
    for (int i = 0; i < 3; i++) pthread_create(&threads[i], NULL, spin, NULL);
    while (now(CLOCK_MONOTONIC) - wall0 < 2.0) usleep(10000);
    STEP("measure");
    struct rusage self, thread; getrusage(RUSAGE_SELF, &self);
    double process = now(CLOCK_PROCESS_CPUTIME_ID), stat = proc_stat_seconds(), mine = 0;
    struct tms t; times(&t); double ticks = (t.tms_utime + t.tms_stime) / (double)sysconf(_SC_CLK_TCK);
    getrusage(RUSAGE_THREAD, &thread); mine = tv(thread.ru_utime) + tv(thread.ru_stime);
    STEP("stop");
    atomic_store(&stop, 1);
    for (int i = 0; i < 3; i++) pthread_join(threads[i], NULL);
    STEP("joined");
    double self_s = tv(self.ru_utime) + tv(self.ru_stime);
    /* Three spinning threads for ~2 s: well above one thread's worth. */
    check("getrusage(RUSAGE_SELF) counts all threads", self_s > 3.0, self_s);
    check("CLOCK_PROCESS_CPUTIME_ID counts all threads", process > 3.0, process);
    check("/proc/self/stat utime+stime counts all threads", stat > 3.0, stat);
    check("times() counts all threads", ticks > 3.0, ticks);
    check("RUSAGE_THREAD is the calling (sleeping) thread only", mine < 1.0, mine);
    STEP("fork");
    pid_t child = fork();
    if (child == 0) { double c0 = now(CLOCK_MONOTONIC); volatile unsigned long x = 0; while (now(CLOCK_MONOTONIC) - c0 < 1.0) x++; _exit(0); }
    struct tms before; clock_t elapsed0 = times(&before);
    check("times() children are zero before reaping", before.tms_cutime + before.tms_cstime == 0, (double)(before.tms_cutime + before.tms_cstime));
    STEP("wait");
    waitpid(child, NULL, 0); struct tms after; times(&after); struct rusage children; getrusage(RUSAGE_CHILDREN, &children);
    double cticks = (after.tms_cutime + after.tms_cstime) / (double)sysconf(_SC_CLK_TCK);
    double elapsed = (times(&after) - elapsed0) / (double)sysconf(_SC_CLK_TCK);
    check("times() return value measures elapsed time", elapsed > 0.5 && elapsed < 30, elapsed);
    check("child CPU time is its own (not the parent's threads)", tv(children.ru_utime) + tv(children.ru_stime) < 3.0, tv(children.ru_utime) + tv(children.ru_stime));
    check("times() children after reaping", cticks > 0.5, cticks);
    check("getrusage(RUSAGE_CHILDREN) after reaping", tv(children.ru_utime) + tv(children.ru_stime) > 0.5, tv(children.ru_utime) + tv(children.ru_stime));
    printf("PROCESS_CPU_TIME_CONTRACT %s failures=%d\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
