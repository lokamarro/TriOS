#define _GNU_SOURCE
// O_PATH descriptors (Qt 6 QProcess opens its working directory this way) and
// Linux default pty termios. Runs as a normal user when started as root.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <termios.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" errno=%d\n", errno); } } while (0)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getuid() == 0 && (setresgid(1000, 1000, 1000) || setresuid(1000, 1000, 1000))) { perror("drop"); return 2; }
    char base[] = "/tmp/lr-opath-XXXXXX";
    CHECK(mkdtemp(base) != NULL, "mkdtemp");
    char dir[256], file[256], child[256];
    snprintf(dir, sizeof dir, "%s/noread", base);
    snprintf(child, sizeof child, "%s/noread/inside", base);
    snprintf(file, sizeof file, "%s/secret", base);
    CHECK(mkdir(dir, 0700) == 0 && close(open(child, O_CREAT | O_WRONLY, 0644)) == 0, "setup dir");
    CHECK(chmod(dir, 0311) == 0, "chmod dir wx only");
    int fdf = open(file, O_CREAT | O_WRONLY, 0600); CHECK(fdf >= 0 && write(fdf, "top", 3) == 3, "setup file"); close(fdf);
    CHECK(chmod(file, 0000) == 0, "chmod file 0");

    errno = 0; CHECK(open(dir, O_RDONLY | O_DIRECTORY) == -1 && errno == EACCES, "unreadable dir O_RDONLY must be EACCES");
    int d = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
    CHECK(d >= 0, "O_PATH on a directory without read permission");
    int flags = fcntl(d, F_GETFL);
    CHECK(flags >= 0 && (flags & O_PATH), "F_GETFL reports O_PATH (%#x)", flags);
    struct stat st; CHECK(fstat(d, &st) == 0 && S_ISDIR(st.st_mode), "fstat through O_PATH");
    char buf[64];
    errno = 0; CHECK(read(d, buf, sizeof buf) == -1 && errno == EBADF, "read on O_PATH must be EBADF");
    errno = 0; CHECK(syscall(SYS_getdents64, d, buf, sizeof buf) == -1 && errno == EBADF, "getdents on O_PATH must be EBADF");
    errno = 0; CHECK(ioctl(d, FIONREAD, &flags) == -1 && errno == EBADF, "ioctl on O_PATH must be EBADF");
    errno = 0; CHECK(lseek(d, 0, SEEK_SET) == -1 && errno == EBADF, "lseek on O_PATH must be EBADF");
    int in = openat(d, "inside", O_RDONLY);
    CHECK(in >= 0, "openat relative to an O_PATH directory (search permission)");
    if (in >= 0) close(in);
    CHECK(fchdir(d) == 0, "fchdir to an O_PATH directory");
    CHECK(chdir("/") == 0, "chdir back");
    close(d);

    errno = 0; CHECK(open(file, O_RDONLY) == -1 && errno == EACCES, "mode 0000 file O_RDONLY must be EACCES");
    int f = open(file, O_PATH);
    CHECK(f >= 0, "O_PATH on a mode-0000 file");
    errno = 0; CHECK(f < 0 || (read(f, buf, sizeof buf) == -1 && errno == EBADF), "no read through O_PATH");
    CHECK(f < 0 || (fstat(f, &st) == 0 && st.st_size == 3), "fstat size %lld", (long long) st.st_size);
    if (f >= 0) close(f);

    char missing[256]; snprintf(missing, sizeof missing, "%s/created", base);
    errno = 0; CHECK(open(missing, O_PATH | O_CREAT, 0644) == -1 && errno == ENOENT, "O_PATH ignores O_CREAT");
    CHECK(access(missing, F_OK) == -1, "nothing created");

    // Default pty termios: B38400, CS8, CREAD (as Linux tty_std_termios).
    int master, slave;
    CHECK(openpty(&master, &slave, NULL, NULL, NULL) == 0, "openpty");
    struct termios t; CHECK(tcgetattr(slave, &t) == 0, "tcgetattr");
    CHECK(cfgetospeed(&t) == B38400 && (t.c_cflag & CSIZE) == CS8 && (t.c_cflag & CREAD), "pty cflag %#x", (unsigned) t.c_cflag);

    chmod(dir, 0700); unlink(child); rmdir(dir); chmod(file, 0600); unlink(file); rmdir(base);
    printf("o_path_contract %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
