#define _GNU_SOURCE
// Linux AF_UNIX stream sendmsg with SCM_RIGHTS under send-buffer pressure:
// nonblocking callers see EAGAIN or a short write, never EMSGSIZE (Chromium
// Mojo treats EMSGSIZE as a fatal channel write error on Linux), and every
// byte and descriptor still arrives in order. Blocking callers get full writes.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MESSAGE 232
static int failures;
static const char *volatile phase = "setup";
static void on_timeout(int sig) {
    (void) sig;
    char line[96]; int n = snprintf(line, sizeof line, "FAIL timeout in %s phase\n", phase);
    if (n > 0) (void) !write(1, line, (size_t) n);
    _exit(3);
}

static ssize_t send_with_fd(int sock, const unsigned char *data, size_t size, int fd, int flags) {
    struct iovec iov = {(void *) data, size};
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control};
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    return sendmsg(sock, &msg, flags);
}

// Receives until EAGAIN (nonblocking) or `want` bytes; checks the byte pattern.
static void drain(int sock, size_t want, int block, size_t *bytes, unsigned *fds) {
    unsigned char buf[4096];
    while (*bytes < want || !block) {
        char control[CMSG_SPACE(16 * sizeof(int))];
        struct iovec iov = {buf, sizeof buf};
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control};
        ssize_t n = recvmsg(sock, &msg, block ? 0 : MSG_DONTWAIT);
        if (n < 0 && !block && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n <= 0) { printf("FAIL recvmsg=%zd errno=%d\n", n, errno); failures++; return; }
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] != (unsigned char) ((*bytes + (size_t) i) % 251)) {
                printf("FAIL byte %zu out of order\n", *bytes + (size_t) i); failures++; return;
            }
        *bytes += (size_t) n;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
                for (size_t k = 0; k < (c->cmsg_len - CMSG_LEN(0)) / sizeof(int); k++) {
                    int fd; memcpy(&fd, CMSG_DATA(c) + k * sizeof(int), sizeof fd);
                    close(fd); (*fds)++;
                }
        if (msg.msg_flags & MSG_CTRUNC) { printf("FAIL MSG_CTRUNC\n"); failures++; }
    }
}

static unsigned char pattern[1 << 20];
static void fill_pattern(void) { for (size_t i = 0; i < sizeof pattern; i++) pattern[i] = (unsigned char) (i % 251); }

static void nonblocking_pressure(void) {
    int sv[2], pipefd[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) || pipe(pipefd)) { perror("setup"); exit(2); }
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    size_t sent = 0; unsigned calls = 0, emsgsize = 0, eagain = 0;
    for (unsigned i = 0; i < 100000 && eagain == 0; i++) {
        ssize_t n = send_with_fd(sv[0], pattern + sent, MESSAGE, pipefd[0], 0);
        if (n > 0) { sent += (size_t) n; calls++; continue; }
        if (errno == EMSGSIZE) emsgsize++;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) eagain++;
        else { printf("FAIL nonblocking sendmsg errno=%d\n", errno); failures++; break; }
        if (emsgsize) break;
    }
    size_t got = 0; unsigned fds = 0;
    drain(sv[1], sent, 0, &got, &fds);
    int ok = emsgsize == 0 && eagain == 1 && got == sent && fds == calls;
    printf("nonblocking %s accepted_calls=%u bytes=%zu emsgsize=%u eagain=%u received=%zu fds=%u\n",
        ok ? "PASS" : "FAIL", calls, sent, emsgsize, eagain, got, fds);
    if (!ok) failures++;
    close(sv[0]); close(sv[1]); close(pipefd[0]); close(pipefd[1]);
}

struct reader { int sock; size_t want, got; unsigned fds; };
static void *reader_main(void *arg) {
    struct reader *r = arg;
    drain(r->sock, r->want, 1, &r->got, &r->fds);
    return NULL;
}

static void blocking_pressure(void) {
    int sv[2], pipefd[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) || pipe(pipefd)) { perror("setup"); exit(2); }
    const unsigned count = 3000;
    struct reader r = {.sock = sv[1], .want = (size_t) count * MESSAGE};
    pthread_t thread; pthread_create(&thread, NULL, reader_main, &r);
    unsigned short_writes = 0, errors = 0; size_t sent = 0;
    for (unsigned i = 0; i < count; i++) {
        ssize_t n = send_with_fd(sv[0], pattern + sent, MESSAGE, pipefd[0], 0);
        if (n < 0) { errors++; printf("FAIL blocking sendmsg errno=%d\n", errno); break; }
        if ((size_t) n != MESSAGE) short_writes++;
        sent += (size_t) n;
    }
    pthread_join(thread, NULL);
    int ok = errors == 0 && short_writes == 0 && r.got == sent && r.fds == count;
    printf("blocking %s messages=%u bytes=%zu short_writes=%u received=%zu fds=%u\n",
        ok ? "PASS" : "FAIL", count, sent, short_writes, r.got, r.fds);
    if (!ok) failures++;
    close(sv[0]); close(sv[1]); close(pipefd[0]); close(pipefd[1]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_timeout);
    fill_pattern();
    phase = "nonblocking"; alarm(60);
    nonblocking_pressure();
    phase = "blocking"; alarm(120);
    blocking_pressure();
    alarm(0);
    printf("unix_rights_backpressure_contract %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
