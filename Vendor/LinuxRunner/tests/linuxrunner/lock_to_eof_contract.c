#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifndef F_OFD_GETLK
#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#endif
#define CHECK(x) do { if (!(x)) { perror(#x); return 1; } } while (0)
int main(void) {
    char name[] = "/tmp/lr-lock-eof-XXXXXX";
    int a = mkstemp(name), b = open(name, O_RDWR);
    CHECK(a >= 0 && b >= 0); CHECK(unlink(name) == 0);
    struct flock whole = {.l_type=F_WRLCK, .l_whence=SEEK_SET, .l_start=0, .l_len=0};
    CHECK(fcntl(a, F_OFD_SETLK, &whole) == 0);
    struct flock last = {.l_type=F_WRLCK, .l_whence=SEEK_SET, .l_start=INT64_MAX, .l_len=1};
    struct flock query = last;
    CHECK(fcntl(b, F_OFD_GETLK, &query) == 0);
    CHECK(query.l_type == F_WRLCK && query.l_pid == -1 && query.l_start == 0 && query.l_len == 0);
    errno=0; CHECK(fcntl(b, F_OFD_SETLK, &last) == -1 && errno == EAGAIN);
    CHECK(close(a) == 0);
    CHECK(fcntl(b, F_OFD_SETLK, &last) == 0);
    struct flock bad = last;
    bad.l_len = 2; errno = 0;
    CHECK(fcntl(b, F_OFD_SETLK, &bad) == -1 && errno == EOVERFLOW);
    bad.l_start = -1; bad.l_len = 0; errno = 0;
    CHECK(fcntl(b, F_OFD_SETLK, &bad) == -1 && errno == EINVAL);
    bad.l_start = 0; bad.l_len = INT64_MIN; errno = 0;
    CHECK(fcntl(b, F_OFD_SETLK, &bad) == -1 && errno == EINVAL);
    bad.l_start = 10; bad.l_len = -5;
    CHECK(fcntl(b, F_OFD_SETLK, &bad) == 0);
    CHECK(close(b) == 0);
    puts("LOCK_TO_EOF_PASS largest guest offset, conflict reflection, invalid/overflow/reversed ranges and close release");
    return 0;
}
