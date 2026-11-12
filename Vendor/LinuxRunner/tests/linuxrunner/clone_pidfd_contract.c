#define _GNU_SOURCE
// clone(CLONE_PIDFD) and clone3's pidfd field, as Qt 6 QProcess (forkfd) uses
// them after waitid(P_PIDFD) reports pidfd support; without them QProcess
// could not start any program and Telegram Desktop ended in kill(-1, SIGKILL).
// Also: kill(-1) never signals the caller's own thread group.
#include <errno.h>
#include <fcntl.h>
#include <linux/sched.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef P_PIDFD
#define P_PIDFD 3
#endif
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" errno=%d\n", errno); } } while (0)

static volatile int usr1_hits;
static void on_usr1(int s) { (void) s; usr1_hits++; }
static void *killer_thread(void *a) { (void) a; kill(-1, SIGUSR1); return NULL; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    // forkfd's probe: waitid(P_PIDFD) on an invalid fd is EBADF when supported.
    siginfo_t si;
    errno = 0;
    CHECK(waitid(P_PIDFD, 0x7fffffff, &si, WEXITED | WNOHANG) == -1 && errno == EBADF, "P_PIDFD probe");

    int pidfd = -1;
    long pid = syscall(SYS_clone, CLONE_PIDFD | SIGCHLD, 0, &pidfd, 0, 0);
    if (pid == 0) { usleep(50000); _exit(23); }
    CHECK(pid > 0 && pidfd >= 0, "clone(CLONE_PIDFD) pid=%ld pidfd=%d", pid, pidfd);
    CHECK(pidfd < 0 || (fcntl(pidfd, F_GETFD) & FD_CLOEXEC), "pidfd is close-on-exec");
    struct pollfd p = {pidfd, POLLIN, 0};
    CHECK(poll(&p, 1, 5000) == 1 && (p.revents & POLLIN), "pidfd readable on exit (revents %#x)", p.revents);
    memset(&si, 0, sizeof si);
    CHECK(waitid(P_PIDFD, pidfd, &si, WEXITED) == 0 && si.si_pid == pid && si.si_status == 23, "waitid via clone pidfd status=%d", si.si_status);
    close(pidfd);

    // clone3 with the pidfd field.
    int pidfd3 = -1;
    struct clone_args args = {.flags = CLONE_PIDFD, .pidfd = (uint64_t)(uintptr_t) &pidfd3, .exit_signal = SIGCHLD};
    long pid3 = syscall(SYS_clone3, &args, sizeof args);
    if (pid3 == 0) _exit(31);
    CHECK(pid3 > 0 && pidfd3 >= 0, "clone3 CLONE_PIDFD pid=%ld pidfd=%d", pid3, pidfd3);
    memset(&si, 0, sizeof si);
    CHECK(waitid(P_PIDFD, pidfd3, &si, WEXITED) == 0 && si.si_status == 31, "clone3 pidfd status=%d", si.si_status);
    close(pidfd3);

    // Invalid combinations.
    int dummy;
    errno = 0;
    CHECK(syscall(SYS_clone, CLONE_PIDFD | CLONE_PARENT_SETTID | SIGCHLD, 0, &dummy, 0, 0) == -1 && errno == EINVAL, "CLONE_PIDFD|CLONE_PARENT_SETTID must be EINVAL");
    errno = 0;
    CHECK(syscall(SYS_clone, CLONE_PIDFD | CLONE_DETACHED | SIGCHLD, 0, &dummy, 0, 0) == -1 && errno == EINVAL, "CLONE_PIDFD|CLONE_DETACHED must be EINVAL");

    // kill(-1) reaches other processes but not the caller's thread group. Run
    // it inside a private PID namespace (members only), from a non-leader
    // thread, so nothing outside the test is signalled.
    pid_t ns = (pid_t) syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWPID | SIGCHLD, 0, 0, 0, 0);
    if (ns == 0) {
        signal(SIGUSR1, on_usr1);
        int ready[2]; if (pipe(ready)) _exit(10);
        pid_t member = fork();
        if (member == 0) { char c = 'r'; usr1_hits = 0; if (write(ready[1], &c, 1) != 1) _exit(3); for (int i = 0; i < 300 && !usr1_hits; i++) usleep(10000); _exit(usr1_hits ? 0 : 1); }
        char c; if (read(ready[0], &c, 1) != 1) _exit(11);
        pthread_t t; pthread_create(&t, NULL, killer_thread, NULL); pthread_join(t, NULL);
        int st = 0; waitpid(member, &st, 0);
        usleep(100000);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) _exit(12);   // member not signalled
        _exit(usr1_hits == 0 ? 0 : 13);                           // caller's group must be spared
    }
    int nst = 0;
    CHECK(ns > 0 && waitpid(ns, &nst, 0) == ns, "namespace child");
    CHECK(WIFEXITED(nst) && WEXITSTATUS(nst) == 0, "kill(-1) semantics in namespace: status %#x (12: member missed, 13: caller hit)", nst);

    printf("clone_pidfd_contract %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
