#define _GNU_SOURCE
// sync_file_range(2) as dpkg and databases use it: regular files succeed for
// write-initiation and for waiting forms, invalid flags and negative ranges are
// EINVAL, pipes ESPIPE, closed descriptors EBADF. Previously an ENOSYS stub.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" errno=%d\n", errno); } } while (0)

int main(void) {
    char path[] = "/tmp/lr-sfr-XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0, "mkstemp");
    CHECK(write(fd, "data", 4) == 4, "write");
    CHECK(sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE) == 0, "write-initiation");
    CHECK(sync_file_range(fd, 0, 4, SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER) == 0, "waiting form");
    CHECK(sync_file_range(fd, 0, 0, 0) == 0, "no flags is a no-op");
    errno = 0; CHECK(sync_file_range(fd, 0, 0, 8) == -1 && errno == EINVAL, "unknown flag must be EINVAL");
    errno = 0; CHECK(sync_file_range(fd, -1, 0, SYNC_FILE_RANGE_WRITE) == -1 && errno == EINVAL, "negative offset must be EINVAL");
    int p[2]; CHECK(pipe(p) == 0, "pipe");
    errno = 0; CHECK(sync_file_range(p[1], 0, 0, SYNC_FILE_RANGE_WRITE) == -1 && errno == ESPIPE, "pipe must be ESPIPE");
    close(fd); unlink(path);
    errno = 0; CHECK(sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE) == -1 && errno == EBADF, "closed fd must be EBADF");
    printf("sync_file_range_contract %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
