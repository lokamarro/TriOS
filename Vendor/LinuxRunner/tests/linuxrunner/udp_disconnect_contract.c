#define _GNU_SOURCE
// connect(AF_UNSPEC) dissolves a datagram socket's association (Linux: 0), and
// dual-stack AF_INET6 sockets accept IPv4 sockaddrs as ::ffff:a.b.c.d. glibc's
// getaddrinfo sorts addresses with both (connect each candidate, read the
// source address, disconnect); EINVAL broke the sort for every resolver user.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" errno=%d\n", errno); } } while (0)

static void run(int family) {
    int s = socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0); CHECK(s >= 0, "socket %d", family);
    struct sockaddr_storage peer = {0}; socklen_t len;
    if (family == AF_INET) { struct sockaddr_in *a = (void *) &peer; a->sin_family = AF_INET; a->sin_port = htons(9); a->sin_addr.s_addr = htonl(INADDR_LOOPBACK); len = sizeof(*a); }
    else { struct sockaddr_in6 *a = (void *) &peer; a->sin6_family = AF_INET6; a->sin6_port = htons(9); a->sin6_addr = in6addr_loopback; len = sizeof(*a); }
    CHECK(connect(s, (void *) &peer, len) == 0, "connect family %d", family);
    struct sockaddr_storage got; socklen_t glen = sizeof got;
    CHECK(getpeername(s, (void *) &got, &glen) == 0, "getpeername when connected");
    struct sockaddr unspec = {.sa_family = AF_UNSPEC};
    CHECK(connect(s, &unspec, sizeof unspec) == 0, "connect(AF_UNSPEC) must disconnect with 0 (family %d)", family);
    glen = sizeof got; errno = 0;
    CHECK(getpeername(s, (void *) &got, &glen) == -1 && errno == ENOTCONN, "getpeername after disconnect must be ENOTCONN");
    errno = 0;
    CHECK(send(s, "x", 1, 0) == -1 && errno == EDESTADDRREQ, "send after disconnect must be EDESTADDRREQ");
    CHECK(connect(s, &unspec, sizeof unspec) == 0, "disconnecting an unconnected socket is 0 too");
    close(s);
}

static void mapped_v4(void) {
    // UDP: AF_INET6 socket connected to 127.0.0.1 reports the mapped peer.
    int u = socket(AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in v4 = {.sin_family = AF_INET, .sin_port = htons(9), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(connect(u, (void *) &v4, sizeof v4) == 0, "AF_INET6 UDP socket connect to IPv4 address");
    struct sockaddr_in6 got = {0}; socklen_t len = sizeof got;
    CHECK(getpeername(u, (void *) &got, &len) == 0 && got.sin6_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&got.sin6_addr),
          "peer is ::ffff:127.0.0.1 (family %d)", got.sin6_family);
    // UDP sendto with an IPv4 sockaddr on an AF_INET6 socket (udpv6_sendmsg).
    u = socket(AF_INET6, SOCK_DGRAM, 0);
    CHECK(sendto(u, "x", 1, 0, (void *) &v4, sizeof v4) == 1, "AF_INET6 UDP sendto IPv4 address");
    // bind() takes only sockaddr_in6 on an AF_INET6 socket (inet6_bind: EINVAL
    // for the 16-byte sockaddr_in).
    struct sockaddr_in any4 = {.sin_family = AF_INET};
    errno = 0;
    CHECK(bind(u, (void *) &any4, sizeof any4) == -1 && errno == EINVAL, "AF_INET6 bind with sockaddr_in errno=%d", errno);
    close(u);
    // IPV6_V6ONLY sockets refuse IPv4 destinations.
    u = socket(AF_INET6, SOCK_DGRAM, 0); int one = 1;
    setsockopt(u, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
    errno = 0;
    CHECK(connect(u, (void *) &v4, sizeof v4) == -1 && errno == EAFNOSUPPORT, "V6ONLY UDP connect to IPv4 errno=%d", errno);
    errno = 0;
    CHECK(sendto(u, "x", 1, 0, (void *) &v4, sizeof v4) == -1 && errno == ENETUNREACH, "V6ONLY UDP sendto IPv4 errno=%d", errno);
    close(u);
    // TCP: IPv4 listener; an AF_INET6 client must use the mapped address
    // (tcp_v6_connect rejects a 16-byte sockaddr_in with EINVAL).
    int l = socket(AF_INET, SOCK_STREAM, 0); int on = 1; setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in la = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(l, (void *) &la, sizeof la) == 0 && listen(l, 1) == 0, "listener");
    len = sizeof la; getsockname(l, (void *) &la, &len);
    int c = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
    errno = 0;
    CHECK(connect(c, (void *) &la, sizeof la) == -1 && errno == EINVAL, "AF_INET6 TCP connect with sockaddr_in errno=%d", errno);
    struct sockaddr_in6 la6 = {.sin6_family = AF_INET6, .sin6_port = la.sin_port};
    la6.sin6_addr.s6_addr[10] = 0xff; la6.sin6_addr.s6_addr[11] = 0xff;
    memcpy(&la6.sin6_addr.s6_addr[12], &la.sin_addr, 4);
    int r = connect(c, (void *) &la6, sizeof la6);
    CHECK(r == 0 || errno == EINPROGRESS, "non-blocking AF_INET6 connect to ::ffff:127.0.0.1 r=%d", r);
    struct pollfd p = {c, POLLOUT, 0};
    CHECK(poll(&p, 1, 5000) == 1 && (p.revents & POLLOUT), "connect completes through poll (revents %#x)", p.revents);
    int soerr = -1; len = sizeof soerr;
    CHECK(getsockopt(c, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 && soerr == 0, "SO_ERROR %d", soerr);
    int a = accept(l, NULL, NULL); CHECK(a >= 0, "accept");
    CHECK(write(c, "ok", 2) == 2, "write");
    char buf[2]; CHECK(read(a, buf, 2) == 2 && buf[0] == 'o', "read");
    close(a); close(c); close(l);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    run(AF_INET);
    run(AF_INET6);
    mapped_v4();
    // The glibc path itself: resolving with AI_ADDRCONFIG-style sorting must work.
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *res = NULL;
    CHECK(getaddrinfo("localhost", "80", &hints, &res) == 0 && res != NULL, "getaddrinfo localhost");
    if (res) freeaddrinfo(res);
    printf("udp_disconnect_contract %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
