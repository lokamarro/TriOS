// O_PATH|O_NOFOLLOW symlink descriptors, readlinkat(fd, ""), statx LR_STATX_MNT_ID /
// LR_STATX_ATTR_MOUNT_ROOT and /proc/self/mountinfo agreement, and procfs readdir inodes.
// systemd 256+ (Arch Linux ARM's systemd-tmpfiles/sysusers) depends on all of them.
// Prints OPATH_STATX_CONTRACT PASS|FAIL with one line per failed check.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
// Kernel struct statx layout (uapi linux/stat.h), declared here because musl's
// <sys/stat.h> and <linux/stat.h> cannot be included together.
struct lr_statx_timestamp { int64_t tv_sec; uint32_t tv_nsec; int32_t reserved; };
struct lr_statx {
    uint32_t stx_mask, stx_blksize; uint64_t stx_attributes; uint32_t stx_nlink, stx_uid, stx_gid;
    uint16_t stx_mode, spare0; uint64_t stx_ino, stx_size, stx_blocks, stx_attributes_mask;
    struct lr_statx_timestamp stx_atime, stx_btime, stx_ctime, stx_mtime;
    uint32_t stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
    uint64_t stx_mnt_id, spare2[13];
};
#define LR_STATX_BASIC_STATS 0x7ffU
#define LR_STATX_MNT_ID 0x1000U
#define LR_STATX_ATTR_MOUNT_ROOT 0x2000ULL

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL " __VA_ARGS__); printf(" (errno=%d)\n", errno); } } while (0)

static int do_statx(int dirfd, const char *path, int flags, unsigned mask, struct lr_statx *sx) {
    return syscall(SYS_statx, dirfd, path, flags, mask, sx);
}

static unsigned long long mountinfo_id(const char *point) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f) return 0;
    char line[4096]; unsigned long long id = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long mid, parent; char root[1024], mp[1024];
        if (sscanf(line, "%llu %llu %*s %1023s %1023s", &mid, &parent, root, mp) == 4 && strcmp(mp, point) == 0)
            id = mid;
    }
    fclose(f);
    return id;
}

int main(void) {
    char dir[] = "/tmp/opath-statx-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp");
    char link[256], target[] = "/usr/lib/os-release-or-anything";
    snprintf(link, sizeof link, "%s/link", dir);
    CHECK(symlink(target, link) == 0, "symlink");

    // O_PATH|O_NOFOLLOW opens the link itself; plain O_NOFOLLOW is ELOOP.
    int fd = open(link, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    CHECK(fd >= 0, "open O_PATH|O_NOFOLLOW on symlink");
    errno = 0;
    CHECK(open(link, O_RDONLY | O_NOFOLLOW) < 0 && errno == ELOOP, "open O_NOFOLLOW on symlink is ELOOP");
    if (fd >= 0) {
        struct stat st;
        CHECK(fstat(fd, &st) == 0 && S_ISLNK(st.st_mode), "fstat O_PATH symlink fd is S_IFLNK");
        CHECK(fstatat(fd, "", &st, AT_EMPTY_PATH) == 0 && S_ISLNK(st.st_mode), "fstatat(fd, \"\", AT_EMPTY_PATH)");
        char buf[256] = {0};
        ssize_t n = readlinkat(fd, "", buf, sizeof buf - 1);
        CHECK(n == (ssize_t)strlen(target) && memcmp(buf, target, n) == 0, "readlinkat(fd, \"\") = '%s'", buf);
        char proc[64], via[256] = {0};
        snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
        CHECK(readlink(proc, via, sizeof via - 1) > 0 && strcmp(via, link) == 0, "/proc/self/fd names the link: '%s'", via);
        errno = 0;
        CHECK(read(fd, buf, 1) < 0 && errno == EBADF, "read on O_PATH fd is EBADF");
        close(fd);
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    errno = 0;
    CHECK(readlinkat(dfd, "", (char[8]){0}, 8) < 0 && errno == ENOENT, "readlinkat(dirfd, \"\") is ENOENT");
    close(dfd);

    // statx: LR_STATX_MNT_ID matches mountinfo; mount roots carry LR_STATX_ATTR_MOUNT_ROOT.
    struct lr_statx sx;
    CHECK(do_statx(AT_FDCWD, "/", 0, LR_STATX_BASIC_STATS | LR_STATX_MNT_ID, &sx) == 0, "statx /");
    CHECK(sx.stx_mask & LR_STATX_MNT_ID, "statx returns LR_STATX_MNT_ID");
    CHECK(sx.stx_mnt_id == mountinfo_id("/"), "statx / mnt_id %llu == mountinfo %llu",
          (unsigned long long)sx.stx_mnt_id, mountinfo_id("/"));
    CHECK((sx.stx_attributes_mask & LR_STATX_ATTR_MOUNT_ROOT) && (sx.stx_attributes & LR_STATX_ATTR_MOUNT_ROOT), "/ is a mount root");
    unsigned long long root_id = sx.stx_mnt_id;
    CHECK(do_statx(AT_FDCWD, "/proc/self", 0, LR_STATX_MNT_ID, &sx) == 0 && sx.stx_mnt_id == mountinfo_id("/proc") &&
          sx.stx_mnt_id != root_id, "statx /proc/self mnt_id matches /proc mount");
    CHECK(do_statx(AT_FDCWD, "/proc", 0, LR_STATX_MNT_ID, &sx) == 0 && (sx.stx_attributes & LR_STATX_ATTR_MOUNT_ROOT), "/proc is a mount root");
    CHECK(do_statx(AT_FDCWD, "/usr", 0, LR_STATX_MNT_ID, &sx) == 0 && sx.stx_mnt_id == root_id &&
          !(sx.stx_attributes & LR_STATX_ATTR_MOUNT_ROOT), "/usr is on / and not a mount root");
    int ufd = open("/usr", O_PATH);
    CHECK(do_statx(ufd, "", AT_EMPTY_PATH, LR_STATX_MNT_ID, &sx) == 0 && sx.stx_mnt_id == root_id, "statx fd AT_EMPTY_PATH mnt_id");
    close(ufd);

    // procfs readdir reports the same nonzero inode as stat.
    DIR *d = opendir("/proc/self");
    struct dirent *e; int entries = 0, zero = 0, mismatched = 0;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        entries++;
        if (e->d_ino == 0) { zero++; continue; }
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && st.st_ino != e->d_ino) mismatched++;
    }
    if (d) closedir(d);
    CHECK(entries > 10 && zero == 0 && mismatched == 0, "/proc/self readdir inodes: entries=%d zero=%d mismatched=%d", entries, zero, mismatched);

    unlink(link); rmdir(dir);
    printf("OPATH_STATX_CONTRACT %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
