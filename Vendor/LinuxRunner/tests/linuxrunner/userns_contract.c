#define _GNU_SOURCE
// Linux unprivileged user namespaces (user_namespaces(7)) as used by the
// Chromium/Electron namespace sandbox: id maps, overflow ids, setgroups,
// capabilities, chroot permission and exec capability recomputation.
// Namespaces are on by default; with ISH_USERNS=0 clone(CLONE_NEWUSER) keeps
// failing with EINVAL (accepted unless LR_REQUIRE_USERNS=1).
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/capability.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" errno=%d\n", errno); } } while (0)

static uint64_t effective_caps(void) {
    struct __user_cap_header_struct h = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct d[2];
    memset(d, 0xff, sizeof d);
    if (syscall(SYS_capget, &h, d) != 0) return ~0ull;
    return d[0].effective | (uint64_t) d[1].effective << 32;
}
static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -errno;
    ssize_t n = write(fd, text, strlen(text));
    int err = n < 0 ? -errno : 0;
    close(fd);
    return err;
}
static void read_link(const char *path, char *out, size_t size) {
    ssize_t n = readlink(path, out, size - 1);
    out[n > 0 ? n : 0] = 0;
}

static int child_main(const char *self, const char *parent_ns) {
    CHECK(getuid() == 65534 && getgid() == 65534, "unmapped ids uid=%d gid=%d", getuid(), getgid());
    CHECK((effective_caps() >> CAP_SYS_ADMIN) & 1, "new namespace lacks CAP_SYS_ADMIN");
    char ns[64]; read_link("/proc/self/ns/user", ns, sizeof ns);
    CHECK(strncmp(ns, "user:[", 6) == 0 && strcmp(ns, parent_ns) != 0, "ns/user %s parent %s", ns, parent_ns);
    char bad[64]; snprintf(bad, sizeof bad, "0 %u 2\n", 1000u);
    CHECK(write_file("/proc/self/uid_map", bad) == -EPERM, "unprivileged two-id map must be EPERM");
    CHECK(write_file("/proc/self/gid_map", "1000 1000 1\n") == -EPERM, "gid_map before setgroups deny must be EPERM");
    CHECK(write_file("/proc/self/setgroups", "deny\n") == 0, "setgroups deny");
    CHECK(write_file("/proc/self/uid_map", "1000 1000 1\n") == 0, "uid_map identity");
    CHECK(write_file("/proc/self/uid_map", "1000 1000 1\n") == -EPERM, "uid_map is write-once");
    CHECK(write_file("/proc/self/gid_map", "1000 1000 1\n") == 0, "gid_map identity");
    CHECK(getuid() == 1000 && geteuid() == 1000 && getgid() == 1000, "mapped ids uid=%d euid=%d gid=%d", getuid(), geteuid(), getgid());
    pid_t helper = fork();
    if (helper == 0) {
        if (chroot("/tmp") != 0) _exit(10);
        if (chdir("/") != 0) _exit(11);
        _exit(access("/bin/sh", F_OK) == 0 ? 12 : 0);
    }
    int status = 0; waitpid(helper, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "chroot inside namespace status=%#x", status);
    CHECK(unshare(CLONE_NEWUSER) == 0, "nested unshare(CLONE_NEWUSER)");
    CHECK(getuid() == 65534 && ((effective_caps() >> CAP_SYS_CHROOT) & 1), "nested namespace uid=%d", getuid());
    fflush(stdout);
    if (failures) return 1;
    execl(self, self, "--report-caps", (char *) NULL);
    printf("FAIL execl errno=%d\n", errno);
    return 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "--report-caps")) {
        uint64_t caps = effective_caps();
        // Non-root (unmapped 65534) inside the namespace: execve clears capabilities.
        if (caps != 0) { printf("FAIL exec kept capabilities %#llx\n", (unsigned long long) caps); return 1; }
        return 0;
    }
    if (getuid() == 0 && (setresgid(1000, 1000, 1000) || setresuid(1000, 1000, 1000))) { perror("drop"); return 2; }
    // Dropping from root clears dumpability (and with it /proc/self ownership), as on Linux.
    if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0) { perror("dumpable"); return 2; }
    char maps[96] = {0}; int fd = open("/proc/self/uid_map", O_RDONLY);
    if (fd >= 0) { ssize_t n = read(fd, maps, sizeof maps - 1); (void) n; close(fd); }
    unsigned a = 1, b = 1, c = 0; sscanf(maps, "%u %u %u", &a, &b, &c);
    CHECK(a == 0 && b == 0 && c == 4294967295u, "initial uid_map '%s'", maps);
    CHECK(effective_caps() == 0, "unprivileged capget %#llx", (unsigned long long) effective_caps());
    CHECK(chroot("/") == -1 && errno == EPERM, "unprivileged chroot must be EPERM");
    char parent_ns[64]; read_link("/proc/self/ns/user", parent_ns, sizeof parent_ns);
    pid_t pid = (pid_t) syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD, 0, 0, 0, 0);
    if (pid < 0) {
        int unavailable = errno == EINVAL;
        const char *require = getenv("LR_REQUIRE_USERNS");
        if (unavailable && !(require && !strcmp(require, "1"))) {
            printf("userns_contract %s (namespaces disabled: clone EINVAL as before)\n", failures ? "FAIL" : "PASS");
            return failures ? 1 : 0;
        }
        printf("FAIL clone(CLONE_NEWUSER) errno=%d\n", errno);
        return 1;
    }
    if (pid == 0) _exit(child_main(argv[0], parent_ns) || failures ? 1 : 0);
    int status = 0; waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "namespace child status=%#x", status);
    printf("userns_contract %s (namespaces enabled)\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
