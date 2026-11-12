#define _GNU_SOURCE
// All guest processes share one host descriptor table. Four processes each hold
// 400 pipes (800 guest fds, inside the guest RLIMIT_NOFILE of 1024) for 3200
// host fds together, above the iOS default host soft limit of 2560. Prints how
// many pipes each child obtained and the first errno; the driver
// (run-host-nofile-macos.py) runs it with and without the startup raise.
// Also checks that the guest limit is independent of the host limit.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

enum { CHILDREN = 4, PIPES = 400 };

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct rlimit guest;
    getrlimit(RLIMIT_NOFILE, &guest);
    printf("guest_nofile soft=%llu hard=%llu\n", (unsigned long long) guest.rlim_cur, (unsigned long long) guest.rlim_max);
    int report[2], hold[2];
    if (pipe(report) || pipe(hold)) { perror("pipe"); return 2; }
    for (int c = 0; c < CHILDREN; c++) {
        if (fork() == 0) {
            close(report[0]); close(hold[1]);
            int made = 0, error = 0, fds[2];
            for (; made < PIPES; made++) if (pipe(fds)) { error = errno; break; }
            int line[3] = {c, made, error};
            if (write(report[1], line, sizeof line) != sizeof line) _exit(3);
            char byte; if (read(hold[0], &byte, 1) < 0) _exit(4);   // hold until all report
            _exit(0);
        }
    }
    close(report[1]); close(hold[0]);
    int total = 0, failed = 0;
    for (int c = 0; c < CHILDREN; c++) {
        int line[3];
        if (read(report[0], line, sizeof line) != sizeof line) { printf("child report missing\n"); return 2; }
        printf("child=%d pipes=%d errno=%d\n", line[0], line[1], line[2]);
        total += line[1]; if (line[2]) failed++;
    }
    close(hold[1]);
    while (wait(NULL) > 0) {}
    printf("host_nofile_pressure total_pipes=%d failed_children=%d %s\n", total, failed,
           failed ? "EXHAUSTED" : "COMPLETE");
    return 0;
}
