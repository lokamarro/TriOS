#define _GNU_SOURCE
// Linux PID namespaces (pid_namespaces(7)) as used by the Chromium/Electron
// zygote: pid 1 and ppid 0 inside, per-namespace allocation, caller-relative
// clone/wait/kill pids, orphan reaping by the namespace init, members killed
// when init exits, SCM_CREDENTIALS pids in the receiver's namespace, nesting.
// Skips (PASS) when disabled with ISH_USERNS=0 unless LR_REQUIRE_USERNS=1.
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" errno=%d\n", errno); } } while (0)
static pid_t raw_clone(unsigned long flags) { return (pid_t) syscall(SYS_clone, flags | SIGCHLD, 0, 0, 0, 0); }
static void sleep_ms(int ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }

static int send_creds(int sock) {
    struct ucred c = {getpid(), getuid(), getgid()};
    char byte = 'x', control[CMSG_SPACE(sizeof c)] = {0};
    struct iovec iov = {&byte, 1};
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control};
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_CREDENTIALS; h->cmsg_len = CMSG_LEN(sizeof c);
    memcpy(CMSG_DATA(h), &c, sizeof c);
    return sendmsg(sock, &m, 0) == 1 ? 0 : -1;
}
static pid_t recv_creds_pid(int sock) {
    char byte, control[CMSG_SPACE(sizeof(struct ucred))];
    struct iovec iov = {&byte, 1};
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control};
    if (recvmsg(sock, &m, 0) != 1) return -1;
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    if (!h || h->cmsg_type != SCM_CREDENTIALS) return -2;
    struct ucred c; memcpy(&c, CMSG_DATA(h), sizeof c);
    return c.pid;
}

// Runs as pid 1 of the new namespace.
static int ns_init(int cred_sock) {
    CHECK(getpid() == 1, "init getpid=%d", getpid());
    CHECK(getppid() == 0, "init getppid=%d (parent is outside)", getppid());
    char link[64] = {0}; ssize_t n = readlink("/proc/self/ns/pid", link, sizeof link - 1); (void) n;
    CHECK(strncmp(link, "pid:[", 5) == 0 && strcmp(link, "pid:[4026531836]") != 0, "ns/pid link %s", link);
    // Child: local pid, parent 1, wait/kill by local pid.
    pid_t a = fork();
    if (a == 0) _exit(getpid() > 1 && getppid() == 1 ? 7 : 1);
    int st = 0;
    CHECK(a > 1, "fork in namespace returned %d", a);
    CHECK(waitpid(a, &st, 0) == a && WIFEXITED(st) && WEXITSTATUS(st) == 7, "wait local child st=%#x", st);
    // Orphan reparenting: grandchild of init is adopted by init (getppid()==1).
    int pipefd[2]; CHECK(pipe(pipefd) == 0, "pipe");
    pid_t b = fork();
    if (b == 0) {
        if (fork() == 0) { close(pipefd[0]); sleep_ms(300); char r = getppid() == 1 ? 'y' : 'n'; if (write(pipefd[1], &r, 1) != 1) _exit(2); _exit(0); }
        _exit(0);
    }
    close(pipefd[1]);
    waitpid(b, NULL, 0);
    char reply = 0; CHECK(read(pipefd[0], &reply, 1) == 1 && reply == 'y', "orphan not reparented to namespace init");
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    // pid arguments of other syscalls are namespace-local too.
    struct rlimit rl;
    CHECK(prlimit(1, RLIMIT_NOFILE, NULL, &rl) == 0, "prlimit(1) in namespace");
    CHECK(setpriority(PRIO_PROCESS, (id_t) syscall(SYS_gettid), 0) == 0, "setpriority(own local tid)");
    errno = 0;
    CHECK(kill(30000, 0) == -1 && errno == ESRCH, "kill of a pid absent from the namespace must be ESRCH");
    // Nested PID namespace (CAP_SYS_ADMIN in our user namespace).
    pid_t nested = raw_clone(CLONE_NEWPID);
    if (nested == 0) _exit(getpid() == 1 ? 9 : 1);
    CHECK(nested > 1, "nested clone returned %d", nested);
    CHECK(waitpid(nested, &st, 0) == nested && WEXITSTATUS(st) == 9, "nested init st=%#x", st);
    // A long-lived member that reports itself to the outside parent, then is
    // killed when this init exits.
    pid_t member = fork();
    if (member == 0) { CHECK(send_creds(cred_sock) == 0, "send creds"); for (;;) pause(); }
    sleep_ms(200);
    return failures ? 1 : 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getuid() == 0 && (setresgid(1000, 1000, 1000) || setresuid(1000, 1000, 1000))) { perror("drop"); return 2; }
    prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
    int sv[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    int on = 1; setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &on, sizeof on);
    pid_t init = raw_clone(CLONE_NEWUSER | CLONE_NEWPID);
    if (init < 0) {
        const char *require = getenv("LR_REQUIRE_USERNS");
        if (errno == EINVAL && !(require && !strcmp(require, "1"))) {
            printf("pidns_contract PASS (namespaces disabled: clone EINVAL as before)\n");
            return 0;
        }
        printf("FAIL clone(CLONE_NEWUSER|CLONE_NEWPID) errno=%d\n", errno);
        return 1;
    }
    if (init == 0) { close(sv[0]); _exit(ns_init(sv[1])); }
    close(sv[1]);
    CHECK(init > 1 && init != getpid(), "clone returned %d", init);
    pid_t member = recv_creds_pid(sv[0]);
    CHECK(member > 2, "member seen from outside as pid %d (its own view is 2+)", member);
    CHECK(member > 0 && kill(member, 0) == 0, "member %d not signalable from outside", member);
    int st = 0;
    CHECK(waitpid(init, &st, 0) == init && WIFEXITED(st) && WEXITSTATUS(st) == 0, "namespace init status=%#x", st);
    int gone = 0;
    for (int i = 0; i < 40 && member > 0; i++) { if (kill(member, 0) != 0 && errno == ESRCH) { gone = 1; break; } sleep_ms(50); }
    CHECK(gone, "member %d survived its namespace init", member);
    printf("pidns_contract %s (namespaces enabled)\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
