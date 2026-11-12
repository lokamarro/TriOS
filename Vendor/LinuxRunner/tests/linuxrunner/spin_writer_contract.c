/* Address-space changes stay live while another thread busy-waits.
 * A guest thread looping in chained code never reached the interpreter's
 * quantum boundary, so it never released mem->lock and every mmap, mprotect,
 * munmap (and pthread_create, which mprotects a guard page) in another thread
 * waited forever. alarm() turns a regression into a failure, not a hang. */
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
static atomic_int stop;
static void *spin(void *arg) { (void)arg; volatile unsigned long x = 0; while (!atomic_load_explicit(&stop, memory_order_relaxed)) x++; return NULL; }
static void *idle(void *arg) { (void)arg; return NULL; }
static void timeout(int sig) { (void)sig; static const char m[] = "SPIN_WRITER_CONTRACT FAIL (timed out: address-space change blocked by a spinning thread)\n"; write(1, m, sizeof m - 1); _exit(1); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(void) {
    signal(SIGALRM, timeout); alarm(60);
    pthread_t spinner, other;
    pthread_create(&spinner, NULL, spin, NULL);
    usleep(200000); // let it settle into its loop
    double t0 = now();
    for (int i = 0; i < 200; i++) {
        char *p = mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { puts("SPIN_WRITER_CONTRACT FAIL mmap"); return 1; }
        p[0] = 1;
        if (mprotect(p, 1 << 16, PROT_READ) || munmap(p, 1 << 16)) { puts("SPIN_WRITER_CONTRACT FAIL mprotect/munmap"); return 1; }
    }
    if (pthread_create(&other, NULL, idle, NULL) || pthread_join(other, NULL)) { puts("SPIN_WRITER_CONTRACT FAIL pthread_create"); return 1; }
    double elapsed = now() - t0;
    atomic_store(&stop, 1); pthread_join(spinner, NULL);
    printf("SPIN_WRITER_CONTRACT PASS (200 mmap/mprotect/munmap + pthread_create beside a busy-wait: %.2f s)\n", elapsed);
    return 0;
}
