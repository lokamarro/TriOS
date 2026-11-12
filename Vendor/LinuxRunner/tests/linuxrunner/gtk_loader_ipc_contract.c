// Linux behavior GLib/glycin image loaders (gdk-pixbuf 2.44, GTK icons) depend on —
// GitHub issue #5 (xfce4-panel abort: "Failed to load .../image-missing.svg: D-Bus error:
// I/O error: Invalid argument (os error 22)"). Three independent EINVALs caused it:
//   1. prctl(PR_SET_PDEATHSIG) unsupported -> Rust process spawn failed
//   2. getsockopt(SO_PEERPIDFD) EINVAL (Linux: a pidfd; ENOPROTOOPT on old kernels)
//   3. recvmsg/recvfrom with a name buffer on a connected socketpair -> EINVAL
//      (Linux: succeeds, address length 0)
//   4. /proc/sys/kernel/overflow{uid,gid} missing -> bubblewrap failed with an error
//      glycin does not recognize, so glycin kept choosing the bwrap sandbox and every
//      loader exited with status 1 (found by the end-to-end xfce4-panel run). With
//      the files present bwrap reports "Creating new namespace failed" and glycin
//      falls back to its unsandboxed loader.
// Prints GTK_LOADER_IPC_CONTRACT PASS|FAIL.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL " __VA_ARGS__); printf(" (errno=%d)\n", errno); } } while (0)

int main(void) {
    // 1. PR_SET_PDEATHSIG / PR_GET_PDEATHSIG and delivery.
    int sig = -1;
    CHECK(prctl(PR_SET_PDEATHSIG, SIGTERM) == 0, "PR_SET_PDEATHSIG");
    CHECK(prctl(PR_GET_PDEATHSIG, &sig) == 0 && sig == SIGTERM, "PR_GET_PDEATHSIG = %d", sig);
    errno = 0;
    CHECK(prctl(PR_SET_PDEATHSIG, 200) < 0 && errno == EINVAL, "invalid signal rejected");
    CHECK(prctl(PR_SET_PDEATHSIG, 0) == 0, "clear");
    int report[2]; CHECK(pipe(report) == 0, "pipe");
    pid_t middle = fork();
    if (middle == 0) {                      // middle: forks a grandchild, then exits
        pid_t g = fork();
        if (g == 0) {
            int inherited = -1; prctl(PR_GET_PDEATHSIG, &inherited);
            prctl(PR_SET_PDEATHSIG, SIGUSR1);
            sigset_t s; sigemptyset(&s); sigaddset(&s, SIGUSR1); sigprocmask(SIG_BLOCK, &s, NULL);
            if (write(report[1], "r", 1) != 1) _exit(3);
            struct timespec t = {5, 0}; int got = sigtimedwait(&s, NULL, &t);
            char result = got == SIGUSR1 && inherited == 0 ? 'Y' : 'N';
            if (write(report[1], &result, 1) != 1) _exit(4);
            _exit(0);
        }
        char c; if (read(report[0], &c, 1) != 1) _exit(5);  // grandchild armed
        _exit(0);                                          // parent death -> SIGUSR1
    }
    waitpid(middle, NULL, 0);
    char result = 0;   // the middle process consumed the grandchild's 'r'
    CHECK(read(report[0], &result, 1) == 1 && result == 'Y', "grandchild got PR_SET_PDEATHSIG signal when its parent exited (%c)", result);

    // 2. SO_PEERPIDFD on a socketpair; unknown options are ENOPROTOOPT.
    int sv[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0, "socketpair");
    int pidfd = -1; socklen_t len = sizeof pidfd;
    CHECK(getsockopt(sv[0], SOL_SOCKET, SO_PEERPIDFD, &pidfd, &len) == 0 && pidfd >= 0 && len == sizeof pidfd, "SO_PEERPIDFD");
    if (pidfd >= 0) {
        char path[64], line[128]; snprintf(path, sizeof path, "/proc/self/fdinfo/%d", pidfd);
        FILE *f = fopen(path, "r"); int pid = -1;
        while (f && fgets(line, sizeof line, f)) sscanf(line, "Pid: %d", &pid);
        if (f) fclose(f);
        CHECK(pid == -1 || pid == getpid(), "SO_PEERPIDFD names the peer process (fdinfo Pid %d)", pid);
        CHECK(syscall(SYS_pidfd_send_signal, pidfd, 0, NULL, 0) == 0, "pidfd usable");
        close(pidfd);
    }
    int v = 0; len = sizeof v; errno = 0;
    CHECK(getsockopt(sv[0], SOL_SOCKET, 9999, &v, &len) < 0 && errno == ENOPROTOOPT, "unknown SOL_SOCKET option is ENOPROTOOPT");

    // 3. Receiving with an address buffer on a connected socketpair.
    CHECK(write(sv[1], "hello", 5) == 5, "write");
    struct sockaddr_un from; memset(&from, 0x55, sizeof from);
    char buf[16]; struct iovec iov = {buf, sizeof buf};
    struct msghdr msg = {.msg_name = &from, .msg_namelen = sizeof from, .msg_iov = &iov, .msg_iovlen = 1};
    ssize_t n = recvmsg(sv[0], &msg, 0);
    CHECK(n == 5 && !memcmp(buf, "hello", 5), "recvmsg with msg_name on socketpair (n=%zd)", n);
    CHECK(msg.msg_namelen == 0 || (msg.msg_namelen >= sizeof(sa_family_t) && from.sun_family == AF_UNIX), "recvmsg address length %u", msg.msg_namelen);
    CHECK(write(sv[1], "x", 1) == 1, "write 2");
    socklen_t flen = sizeof from;
    n = recvfrom(sv[0], buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
    CHECK(n == 1 && (flen == 0 || from.sun_family == AF_UNIX), "recvfrom with address on socketpair (n=%zd len=%u)", n, flen);

    // 4. overflowuid / overflowgid (Linux default 65534).
    const char *overflow[] = {"/proc/sys/kernel/overflowuid", "/proc/sys/kernel/overflowgid"};
    for (int i = 0; i < 2; i++) {
        FILE *o = fopen(overflow[i], "r"); int id = -1;
        if (o) { if (fscanf(o, "%d", &id) != 1) id = -1; fclose(o); }
        CHECK(id == 65534, "%s = %d", overflow[i], id);
    }

    printf("GTK_LOADER_IPC_CONTRACT %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
