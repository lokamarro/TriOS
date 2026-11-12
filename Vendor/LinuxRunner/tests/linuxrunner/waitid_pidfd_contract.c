#define _GNU_SOURCE
// waitid(P_PIDFD) as GLib's child watch uses it (g_child_watch_source_new):
// exit status through a pidfd, PIDFD_NONBLOCK gives EAGAIN while the child
// runs, a non-pidfd descriptor is EINVAL and a closed one EBADF.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef P_PIDFD
#define P_PIDFD 3
#endif
#ifndef PIDFD_NONBLOCK
#define PIDFD_NONBLOCK O_NONBLOCK
#endif
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" errno=%d\n", errno); } } while (0)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int gate[2]; CHECK(pipe(gate) == 0, "pipe");
    pid_t child = fork();
    if (child == 0) { char c; close(gate[1]); if (read(gate[0], &c, 1) < 0) _exit(1); _exit(42); }
    close(gate[0]);
    int pidfd = (int) syscall(SYS_pidfd_open, child, PIDFD_NONBLOCK);
    CHECK(pidfd >= 0, "pidfd_open");
    siginfo_t si; memset(&si, 0, sizeof si);
    errno = 0;
    CHECK(waitid(P_PIDFD, pidfd, &si, WEXITED) == -1 && errno == EAGAIN, "nonblocking pidfd while running must be EAGAIN");
    CHECK(write(gate[1], "x", 1) == 1, "release child");
    int got = -1;
    for (int i = 0; i < 200 && got != 0; i++) {
        memset(&si, 0, sizeof si);
        got = waitid(P_PIDFD, pidfd, &si, WEXITED);
        if (got != 0) usleep(10000);
    }
    CHECK(got == 0 && si.si_pid == child && si.si_code == CLD_EXITED && si.si_status == 42,
          "waitid(P_PIDFD) pid=%d code=%d status=%d", si.si_pid, si.si_code, si.si_status);
    errno = 0;
    CHECK(waitid(P_PIDFD, 0, &si, WEXITED) == -1 && errno == EINVAL, "non-pidfd descriptor must be EINVAL");
    close(pidfd);
    errno = 0;
    CHECK(waitid(P_PIDFD, pidfd, &si, WEXITED) == -1 && errno == EBADF, "closed pidfd must be EBADF");
    // Blocking pidfd: waits for the exit.
    pid_t second = fork();
    if (second == 0) { usleep(100000); _exit(7); }
    int blocking = (int) syscall(SYS_pidfd_open, second, 0);
    memset(&si, 0, sizeof si);
    CHECK(blocking >= 0 && waitid(P_PIDFD, blocking, &si, WEXITED) == 0 && si.si_status == 7, "blocking pidfd status=%d", si.si_status);
    printf("waitid_pidfd_contract %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
