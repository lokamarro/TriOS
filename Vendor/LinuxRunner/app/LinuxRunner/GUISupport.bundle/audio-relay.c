/* LinuxRunner session sound relay: copies PCM from stdin (parec recording the
 * session's null-sink monitor with low latency) to one loopback TCP client,
 * the LinuxRunner app. With no client connected the audio is discarded, so
 * the recorder never blocks. Usage: audio-relay PORT */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) return 2;
    signal(SIGPIPE, SIG_IGN);
    int listener = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (listener < 0) { perror("AUDIO_RELAY socket"); return 1; }
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port)};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* never reachable off the device */
    if (bind(listener, (struct sockaddr *)&address, sizeof address) || listen(listener, 1)) {
        perror("AUDIO_RELAY listen"); return 1;
    }
    fprintf(stderr, "AUDIO_RELAY: listening on 127.0.0.1:%d\n", port);
    int client = -1;
    char buffer[8192];
    for (;;) {
        struct pollfd fds[2] = {{.fd = 0, .events = POLLIN}, {.fd = listener, .events = POLLIN}};
        if (poll(fds, 2, -1) < 0) { if (errno == EINTR) continue; perror("AUDIO_RELAY poll"); return 1; }
        if (fds[1].revents & POLLIN) {
            int next = accept(listener, NULL, NULL);
            if (next >= 0) {
                if (client >= 0) close(client); /* the newest viewer wins */
                client = next;
                setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                fprintf(stderr, "AUDIO_RELAY: client connected\n");
            }
        }
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(0, buffer, sizeof buffer);
            if (n == 0) { fprintf(stderr, "AUDIO_RELAY: recorder ended\n"); return 0; }
            if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; perror("AUDIO_RELAY read"); return 1; }
            for (ssize_t done = 0; client >= 0 && done < n;) {
                ssize_t w = write(client, buffer + done, (size_t)(n - done));
                if (w > 0) done += w;
                else if (w < 0 && errno == EINTR) continue;
                else { close(client); client = -1; fprintf(stderr, "AUDIO_RELAY: client disconnected\n"); }
            }
        }
    }
}
