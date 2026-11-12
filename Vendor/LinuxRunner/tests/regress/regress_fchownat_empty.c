#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { perror(#x); return 1; } } while (0)
int main(void) {
    char directory[] = "/tmp/lr-chown-XXXXXX", path[256], renamed[256];
    CHECK(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/source", directory);
    snprintf(renamed, sizeof(renamed), "%s/renamed", directory);
    int fd = open(path, O_CREAT|O_EXCL|O_RDWR, 0600);
    CHECK(fd >= 0);
    CHECK(fchownat(fd, "", 0, 42, AT_EMPTY_PATH) == 0);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_gid == 42);
    int opath = open(path, O_PATH|O_CLOEXEC);
    CHECK(opath >= 0);
    CHECK(rename(path, renamed) == 0);
    CHECK(fchownat(opath, "", 123, 456, AT_EMPTY_PATH) == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_uid == 123 && st.st_gid == 456);
    CHECK(unlink(renamed) == 0);
    CHECK(fchownat(fd, "", (uid_t)-1, 42, AT_EMPTY_PATH) == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_uid == 123 && st.st_gid == 42);
    errno = 0; CHECK(fchownat(fd, "", 0, 0, 0) == -1 && errno == ENOENT);
    errno = 0; CHECK(fchownat(-1, "", 0, 0, AT_EMPTY_PATH) == -1 && errno == EBADF);
    errno = 0; CHECK(fchownat(fd, "", 0, 0, 0x40000000) == -1 && errno == EINVAL);
    int cwd = open(".", O_RDONLY|O_DIRECTORY);
    CHECK(cwd >= 0 && chdir(directory) == 0);
    CHECK(fchownat(AT_FDCWD, "", 123, 42, AT_EMPTY_PATH) == 0);
    CHECK(stat(".", &st) == 0 && st.st_uid == 123 && st.st_gid == 42);
    CHECK(fchdir(cwd) == 0);
    close(cwd); close(opath); close(fd); CHECK(rmdir(directory) == 0);
    puts("PASS fchownat AT_EMPTY_PATH: fd, O_PATH, rename/unlink, cwd, unchanged uid and error semantics");
    return 0;
}
