// ptrace attach to a process that is not the tracer's child (strace -p,
// gdb -p): PTRACE_SEIZE/INTERRUPT/PEEKDATA/SYSCALL/DETACH and PTRACE_ATTACH
// from a sibling, the real parent staying blind to ptrace stops and unable to
// drive the tracee, and the EPERM/ESRCH cases.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile uint64_t magic = 0x4c52414e4e4552ULL;   // read by the tracer
struct shared { volatile int stop_worker, tracer_done; volatile long worker_ticks; };

static int tracer(pid_t x) {
    int status, stops = 0;
    if (ptrace(PTRACE_SEIZE, x, 0, PTRACE_O_TRACESYSGOOD) != 0) { printf("tracer: SEIZE errno=%d\n", errno); return 10; }
    if (ptrace(PTRACE_SEIZE, x, 0, 0) == 0 || errno != EPERM) { printf("tracer: second SEIZE errno=%d\n", errno); return 11; }
    if (ptrace(PTRACE_INTERRUPT, x, 0, 0) != 0) return 12;
    if (waitpid(x, &status, __WALL) != x || !WIFSTOPPED(status) || (status >> 16) != PTRACE_EVENT_STOP) {
        printf("tracer: interrupt stop status=%#x\n", status); return 13;
    }
    errno = 0;
    long word = ptrace(PTRACE_PEEKDATA, x, (void *) &magic, 0);
    if (errno || (uint64_t) word != magic) { printf("tracer: PEEKDATA %#lx errno=%d\n", word, errno); return 14; }
    for (int i = 0; i < 40; i++) {
        if (ptrace(PTRACE_SYSCALL, x, 0, 0) != 0) return 15;
        if (waitpid(x, &status, __WALL) != x || !WIFSTOPPED(status)) { printf("tracer: syscall stop status=%#x\n", status); return 16; }
        if (WSTOPSIG(status) == (SIGTRAP | 0x80)) stops++;
    }
    if (ptrace(PTRACE_DETACH, x, 0, 0) != 0) { printf("tracer: DETACH errno=%d\n", errno); return 17; }
    if (stops < 20) { printf("tracer: only %d syscall stops\n", stops); return 18; }
    // PTRACE_ATTACH stops the tracee with SIGSTOP.
    if (ptrace(PTRACE_ATTACH, x, 0, 0) != 0) { printf("tracer: ATTACH errno=%d\n", errno); return 19; }
    if (waitpid(x, &status, __WALL) != x || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP) {
        printf("tracer: attach stop status=%#x\n", status); return 20;
    }
    if (ptrace(PTRACE_DETACH, x, 0, 0) != 0) return 21;
    return 0;
}

static void *spin(void *arg) { (void) arg; for (;;) pause(); return NULL; }

int main(void) {
    alarm(60);
    struct shared *sh = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int failures = 0;
    pid_t x = fork();
    if (x == 0) {
        while (!sh->stop_worker) { syscall(SYS_getppid); sh->worker_ticks++; usleep(1000); }
        _exit(magic == 0x4c52414e4e4552ULL ? 5 : 6);
    }
    pid_t t = fork();
    if (t == 0) { int r = tracer(x); sh->tracer_done = 1; _exit(r); }

    // The real parent never sees the sibling's ptrace stops.
    int status, leaked = 0;
    while (!sh->tracer_done) {
        pid_t r = waitpid(x, &status, WNOHANG | WUNTRACED);
        if (r == x) { leaked++; printf("parent saw status %#x for its child\n", status); }
        usleep(500);
    }
    if (leaked) failures++;
    if (waitpid(t, &status, 0) != t || !WIFEXITED(status) || WEXITSTATUS(status)) {
        printf("FAIL tracer status %#x\n", status); failures++;
    }
    long before = sh->worker_ticks; usleep(50000);
    if (sh->worker_ticks == before) { printf("FAIL worker not running after detach\n"); failures++; }

    // A parent cannot drive a child someone else traces; then error cases.
    pthread_t th; pthread_create(&th, NULL, spin, NULL);
    errno = 0;
    if (ptrace(PTRACE_ATTACH, getpid(), 0, 0) == 0 || errno != EPERM) { printf("FAIL self attach errno=%d\n", errno); failures++; }
    if (ptrace(PTRACE_SEIZE, 999999, 0, 0) == 0 || errno != ESRCH) { printf("FAIL missing pid errno=%d\n", errno); failures++; }
    if (ptrace(PTRACE_PEEKDATA, x, (void *) &magic, 0) != -1 || errno != ESRCH) { printf("FAIL untraced child PEEKDATA errno=%d\n", errno); failures++; }

    sh->stop_worker = 1;
    if (waitpid(x, &status, 0) != x || !WIFEXITED(status) || WEXITSTATUS(status) != 5) { printf("FAIL worker exit %#x\n", status); failures++; }
    printf("PTRACE_ATTACH_CONTRACT %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
