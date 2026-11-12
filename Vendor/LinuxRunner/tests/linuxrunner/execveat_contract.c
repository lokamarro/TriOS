#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

static void execat(int dir, const char *path, char **args, int flags) {
    syscall(SYS_execveat, dir, path, args, environ, flags);
}
static void waited(pid_t pid, int expected) {
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expected) {
        fprintf(stderr, "child status=%#x expected=%d\n", status, expected);
        abort();
    }
}
static void expect_errno(int dir, const char *path, int flags, int wanted) {
    char *args[] = {"probe", NULL};
    errno = 0;
    execat(dir, path, args, flags);
    if (errno != wanted) {
        fprintf(stderr, "execveat dir=%d path=%s flags=%#x errno=%d expected=%d\n",
                dir, path ? path : "<null>", flags, errno, wanted);
        abort();
    }
}
static void success(int dir, const char *path, int flags, int offset_fd) {
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        char n[24]; snprintf(n, sizeof(n), "%d", offset_fd);
        char *args[] = {"execveat-probe", "--probe", n, NULL};
        execat(dir, path, args, flags);
        perror("execveat success"); _exit(127);
    }
    waited(child, 73);
}
static void copy_binary(const char *src, const char *dst) {
    int in = open(src, O_RDONLY), out = open(dst, O_WRONLY|O_CREAT|O_EXCL, 0755);
    assert(in >= 0 && out >= 0);
    char buf[8192]; ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) assert(write(out, buf, n) == n);
    assert(n == 0); assert(!close(in)); assert(!close(out));
}
static void check_ids(void) {
    assert(getauxval(AT_UID) == getuid());
    assert(getauxval(AT_EUID) == geteuid());
    assert(getauxval(AT_GID) == getgid());
    assert(getauxval(AT_EGID) == getegid());
    assert(getauxval(AT_SECURE) == (getuid() != geteuid() || getgid() != getegid()));
    uid_t r,e,s; gid_t gr,ge,gs;
    assert(!getresuid(&r,&e,&s) && s == e);
    assert(!getresgid(&gr,&ge,&gs) && gs == ge);
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--probe")) {
        check_ids();
        int fd = atoi(argv[2]);
        if (fd >= 0) assert(lseek(fd, 0, SEEK_CUR) == 37);
        return 73;
    }
    if (argc > 1 && !strcmp(argv[1], "--ids")) {
        assert(geteuid() == 1001 && getegid() == 1002);
        check_ids(); return 74;
    }
    if (argc > 1 && !strcmp(argv[1], "--nnp")) {
        assert(geteuid() == 1003 && getegid() == 1003);
        assert(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1);
        check_ids(); return 79;
    }
    if (argc == 3 && !strcmp(argv[1], "--unchanged-gid")) {
        assert(getegid() == (gid_t)atoi(argv[2])); check_ids(); return 74;
    }
    if (argc == 1 && !argv[0][0]) return 78;
    assert(argc == 1);
    // Linux execveat hands script interpreters /dev/fd/N. A minimal
    // userspace fixture must provide this standard link; do not skip scripts.
    struct stat fd_link;
    if (lstat("/dev/fd", &fd_link) < 0) {
        assert(errno == ENOENT && !symlink("/proc/self/fd", "/dev/fd"));
    }
    assert(!stat("/dev/fd", &fd_link) && S_ISDIR(fd_link.st_mode));
    char dirpath[] = "/tmp/execveat-contract-XXXXXX";
    char binary[160], linkpath[160], script[160];
    assert(mkdtemp(dirpath) && !chmod(dirpath, 0755));
    snprintf(binary, sizeof(binary), "%s/program", dirpath);
    snprintf(linkpath, sizeof(linkpath), "%s/link", dirpath);
    snprintf(script, sizeof(script), "%s/script", dirpath);
    copy_binary(argv[0], binary);
    assert(!symlink("program", linkpath));
    int dir = open(dirpath, O_RDONLY|O_DIRECTORY), fd = open(binary, O_RDONLY);
    assert(dir >= 0 && fd >= 0); assert(lseek(fd, 37, SEEK_SET) == 37);
    success(dir, "program", 0, fd);
    success(-42, binary, 0, fd); // Absolute paths ignore dirfd.
    success(dir, "link", 0, fd);
    expect_errno(dir, "link", AT_SYMLINK_NOFOLLOW, ELOOP);
    expect_errno(dir, "", 0, ENOENT);
    expect_errno(dir, "program", 0x40000000, EINVAL);
    expect_errno(-42, "program", 0, EBADF);
    expect_errno(fd, "program", 0, ENOTDIR);
    expect_errno(dir, NULL, 0, EFAULT);
    expect_errno(dir, "", AT_EMPTY_PATH, EACCES); // Directory not executable.
    success(fd, "", AT_EMPTY_PATH, fd);
    assert(lseek(fd, 0, SEEK_CUR) == 37); // exec reads do not mutate shared offset.
    assert(!fcntl(fd, F_SETFD, FD_CLOEXEC));
    success(fd, "", AT_EMPTY_PATH, -1); // ELF works even with CLOEXEC.
    assert(!fcntl(fd, F_SETFD, 0));

    int sf = open(script, O_WRONLY|O_CREAT|O_EXCL, 0755); assert(sf >= 0);
    char body[128];
    snprintf(body, sizeof(body), "#!/bin/sh\n[ \"$(id -u)\" = %u ] || exit 77\nexit 75\n", (unsigned)geteuid());
    assert(write(sf, body, strlen(body)) == (ssize_t)strlen(body)); assert(!close(sf));
    sf = open(script, O_RDONLY); assert(sf >= 0);
    pid_t child = fork(); assert(child >= 0);
    if (!child) { char *args[] = {"script", NULL}; execat(sf, "", args, AT_EMPTY_PATH); _exit(127); }
    waited(child, 75);
    assert(!fcntl(sf, F_SETFD, FD_CLOEXEC));
    expect_errno(sf, "", AT_EMPTY_PATH, ENOENT);
    assert(!fcntl(dir, F_SETFD, FD_CLOEXEC));
    expect_errno(dir, "script", 0, ENOENT);
    assert(!fcntl(dir, F_SETFD, 0));
    child = fork(); assert(child >= 0);
    if (!child) { char *args[] = {"script", NULL}; execat(dir, "script", args, 0); _exit(127); }
    waited(child, 75);
    child = fork(); assert(child >= 0);
    if (!child) { execat(dir, "program", NULL, 0); _exit(127); }
    waited(child, 78);
    // Set-ID ELF credentials and auxv agree; set-ID script bits are ignored.
    if (getuid() == 0) {
        assert(!chown(script, 1001, 1002) && !chmod(script, 06755));
        child = fork(); assert(child >= 0);
        if (!child) { char *args[] = {script, NULL}; execat(dir, "script", args, 0); _exit(127); }
        waited(child, 75); // Body checks effective identity, not only exit status.
        assert(!chown(binary, 1001, 1002)); assert(!chmod(binary, 06755));
        child = fork(); assert(child >= 0);
        if (!child) { char *args[] = {binary, "--ids", NULL}; execat(dir, "program", args, 0); _exit(127); }
        waited(child, 74);
        child = fork(); assert(child >= 0);
        if (!child) {
            assert(!setgroups(0, NULL) && !setgid(1003) && !setuid(1003));
            assert(!prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
            char *args[] = {binary, "--nnp", NULL};
            execat(dir, "program", args, 0); _exit(127);
        }
        waited(child, 79);
        assert(!chmod(binary, 0750));
        child = fork(); assert(child >= 0);
        if (!child) { assert(!setgid(1002) && !setuid(1001)); char *args[] = {binary, "--ids", NULL}; execat(dir, "program", args, 0); _exit(127); }
        waited(child, 74);
        child = fork(); assert(child >= 0);
        if (!child) { assert(!setgroups(0, NULL) && !setgid(1003) && !setuid(1003)); expect_errno(dir, "program", 0, EACCES); _exit(76); }
        waited(child, 76);
        assert(!chmod(binary, 0100)); // Executable owner does not need read permission.
        child = fork(); assert(child >= 0);
        if (!child) {
            assert(!setgroups(0, NULL) && !setgid(1002) && !setuid(1001));
            char *args[] = {binary, "--ids", NULL};
            execat(dir, "program", args, 0); _exit(127);
        }
        waited(child, 74);
    }
    if (getuid() == 0) {
        assert(!chmod(binary, 02700)); // SGID without group execute is not set-ID execution.
        child=fork(); assert(child >= 0);
        if (!child) { char expected[24]; snprintf(expected,sizeof(expected),"%u",getegid());
            char *args[]={binary,"--unchanged-gid",expected,NULL}; execat(dir,"program",args,0); _exit(127); }
        waited(child,74); assert(!chmod(binary,0755));
    }
    assert(!unlink(binary));
    success(fd, "", AT_EMPTY_PATH, fd); // Load the retained object, not its removed path.
    assert(lseek(fd, 0, SEEK_CUR) == 37);
    assert(!close(sf) && !close(fd) && !close(dir));
    assert(!unlink(linkpath) && !unlink(script) && !rmdir(dirpath));
    puts("EXECVEAT_CONTRACT_PASS relative absolute empty_path deleted_fd offset cloexec_script flags nofollow auxv permissions");
    return 0;
}
