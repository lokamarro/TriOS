/* Socket calls on descriptors that are not sockets. Linux returns ENOTSOCK for
 * an open non-socket and EBADF only for a closed descriptor. PulseAudio's
 * pa_write()/pa_read() try send()/recv() first and fall back to write()/read()
 * only on ENOTSOCK; EBADF there aborted pulseaudio (pa_fdsem_post). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
static int fails;
static void expect(const char *what, long r, int err) {
    int ok = r == -1 && errno == err;
    printf("%s %s (r=%ld errno=%d expected=%d)\n", ok ? "PASS" : "FAIL", what, r, errno, err);
    if (!ok) fails++;
}
int main(void) {
    int p[2]; char c = 'x'; int v = 0; socklen_t len = sizeof v;
    struct sockaddr_storage sa; socklen_t salen = sizeof sa;
    if (pipe(p)) return 2;
    int efd = eventfd(0, 0), file = open("/proc/self/status", O_RDONLY);
    int fds[] = {p[1], efd, file};
    const char *names[] = {"pipe", "eventfd", "file"};
    for (int i = 0; i < 3; i++) {
        char what[64];
        snprintf(what, sizeof what, "send on %s", names[i]); expect(what, send(fds[i], &c, 1, MSG_NOSIGNAL), ENOTSOCK);
        snprintf(what, sizeof what, "recv on %s", names[i]); expect(what, recv(fds[i], &c, 1, MSG_DONTWAIT), ENOTSOCK);
        snprintf(what, sizeof what, "getsockopt on %s", names[i]); expect(what, getsockopt(fds[i], SOL_SOCKET, SO_TYPE, &v, &len), ENOTSOCK);
        snprintf(what, sizeof what, "getpeername on %s", names[i]); expect(what, getpeername(fds[i], (struct sockaddr *)&sa, &salen), ENOTSOCK);
        snprintf(what, sizeof what, "shutdown on %s", names[i]); expect(what, shutdown(fds[i], SHUT_RDWR), ENOTSOCK);
    }
    close(p[0]); close(p[1]);
    expect("send on closed descriptor", send(p[1], &c, 1, MSG_NOSIGNAL), EBADF);
    expect("getsockopt on closed descriptor", getsockopt(p[1], SOL_SOCKET, SO_TYPE, &v, &len), EBADF);
    printf("NOTSOCK_CONTRACT %s failures=%d\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
