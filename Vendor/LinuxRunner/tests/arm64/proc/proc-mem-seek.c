#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static int failures;

static void check_seek(int fd, const char *name, off_t offset, int whence,
        off_t expected, int expected_errno) {
    const int sentinel_errno = 123;
    errno = sentinel_errno;
    off_t actual = lseek(fd, offset, whence);
    int actual_errno = errno;

    if (actual != expected || actual_errno != expected_errno) {
        fprintf(stderr,
                "%s: lseek(%" PRId64 ", %d) = %" PRId64
                " errno=%d; expected %" PRId64 " errno=%d\n",
                name, (int64_t) offset, whence, (int64_t) actual,
                actual_errno, (int64_t) expected, expected_errno);
        failures++;
    }
}

int main(void) {
    _Static_assert(sizeof(off_t) == sizeof(int64_t), "64-bit off_t required");

    int fd = open("/proc/self/mem", O_RDONLY);
    if (fd < 0) {
        perror("open /proc/self/mem");
        return 2;
    }

    check_seek(fd, "set-zero", 0, SEEK_SET, 0, 123);
    check_seek(fd, "set-positive", 4096, SEEK_SET, 4096, 123);
    check_seek(fd, "cur-positive", 16, SEEK_CUR, 4112, 123);
    check_seek(fd, "cur-negative", -1, SEEK_CUR, 4111, 123);

    check_seek(fd, "set-negative", -4096, SEEK_SET, -4096, 123);
    check_seek(fd, "cur-more-negative", -1, SEEK_CUR, -4097, 123);
    check_seek(fd, "end-zero-rejected", 0, SEEK_END, -1, EINVAL);
    check_seek(fd, "end-keeps-position", 0, SEEK_CUR, -4097, 123);
    check_seek(fd, "end-positive-rejected", 1, SEEK_END, -1, EINVAL);
    check_seek(fd, "bad-whence-rejected", 0, 99, -1, EINVAL);
    check_seek(fd, "bad-whence-keeps-position", 0, SEEK_CUR, -4097, 123);

    check_seek(fd, "set-max", INT64_MAX, SEEK_SET, INT64_MAX, 123);
    check_seek(fd, "cur-wrap-positive", 1, SEEK_CUR, INT64_MIN, 123);
    check_seek(fd, "set-min", INT64_MIN, SEEK_SET, INT64_MIN, 123);
    check_seek(fd, "cur-wrap-negative", -1, SEEK_CUR, INT64_MAX, 123);

    /* Linux updates f_pos even when a negative success value is decoded by
     * libc as an errno. Advancing from -1 to +1 proves that state change. */
    check_seek(fd, "set-minus-one", -1, SEEK_SET, -1, EPERM);
    check_seek(fd, "minus-one-was-stored", 2, SEEK_CUR, 1, 123);

    if (close(fd) != 0) {
        perror("close /proc/self/mem");
        return 2;
    }
    if (failures != 0)
        return 1;

    puts("proc-mem-seek-ok");
    return 0;
}
