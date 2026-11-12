#include <string.h>
#include <sys/stat.h>
#include <limits.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/path.h"

// ARM64 stat conversion (128 bytes struct)
struct stat_arm64 stat_convert_arm64(struct statbuf stat) {
    struct stat_arm64 arm64stat = {};
    arm64stat.dev = stat.dev;
    arm64stat.ino = stat.inode;
    arm64stat.mode = stat.mode;
    arm64stat.nlink = stat.nlink;
    arm64stat.uid = stat.uid;
    arm64stat.gid = stat.gid;
    arm64stat.rdev = stat.rdev;
    arm64stat.__pad1 = 0;
    arm64stat.size = stat.size;
    arm64stat.blksize = stat.blksize;
    arm64stat.__pad2 = 0;
    arm64stat.blocks = stat.blocks;
    arm64stat.atime_ = stat.atime;
    arm64stat.atime_nsec = stat.atime_nsec;
    arm64stat.mtime_ = stat.mtime;
    arm64stat.mtime_nsec = stat.mtime_nsec;
    arm64stat.ctime_ = stat.ctime;
    arm64stat.ctime_nsec = stat.ctime_nsec;
    arm64stat.__unused4 = 0;
    arm64stat.__unused5 = 0;
    return arm64stat;
}

// Mount identity for statx: the mount's id and whether the object is that
// mount's root directory (STATX_ATTR_MOUNT_ROOT). known is false when the
// object's path cannot be determined (then no mount-root claim is made).
struct stat_mount_info { uint64_t id; bool known, root; };

static void stat_mount_of_path(const char *normal, struct stat_mount_info *info) {
    char trimmed[MAX_PATH];
    strcpy(trimmed, normal);
    struct mount *mount = find_mount_and_trim_path(trimmed);
    info->id = mount->id;
    info->known = true;
    info->root = trimmed[0] == '\0' || strcmp(trimmed, "/") == 0;
    mount_release(mount);
}

static void stat_mount_of_fd(struct fd *fd, struct stat_mount_info *info) {
    char path[MAX_PATH];
    if (generic_getpath(fd, path) == 0 && path[0] == '/')
        stat_mount_of_path(path, info);
    else
        info->known = false;
    info->id = fd->mount->id;
}

static int generic_statat_mount(struct fd *at, const char *path_raw, struct statbuf *stat, bool follow_links,
        struct stat_mount_info *info) {
    char path[MAX_PATH];
    struct fd *target = NULL;
    int err = path_normalize_target(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW | N_STAT_TARGET : N_SYMLINK_NOFOLLOW, &target);
    if (err < 0)
        return err;
    if (target) {
        err = target->mount->fs->fstat(target, stat);
        if (info != NULL)
            stat_mount_of_fd(target, info);
        fd_close(target);
        return err;
    }
    if (info != NULL)
        stat_mount_of_path(path[0] ? path : "/", info);
    struct mount *mount = find_mount_and_trim_path(path);
    memset(stat, 0, sizeof(*stat));
    err = mount->fs->stat(mount, path, stat);
    mount_release(mount);
    return err;
}

int generic_statat(struct fd *at, const char *path_raw, struct statbuf *stat, bool follow_links) {
    return generic_statat_mount(at, path_raw, stat, follow_links, NULL);
}

// Linux UAPI values (include/uapi/linux/fcntl.h). No automounter or
// remote attribute cache exists here, so AT_NO_AUTOMOUNT and each individual
// statx sync choice use the same local filesystem metadata callbacks.
#define AT_NO_AUTOMOUNT_ 0x800
#define AT_STATX_SYNC_TYPE_ 0x6000
#define STATX_RESERVED_ 0x80000000U

static struct fd *stat_fd_retain(fd_t number) {
    lock(&current->files->lock);
    struct fd *fd = fdtable_get(current->files, number);
    if (fd != NULL)
        fd_retain(fd);
    unlock(&current->files->lock);
    return fd;
}

static int stat_path_at_mount(fd_t at_f, const char *path, dword_t flags,
        struct statbuf *stat, struct stat_mount_info *info) {
    bool follow_links = !(flags & AT_SYMLINK_NOFOLLOW_);
    if (path[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH_))
            return _ENOENT;
        struct fd *fd;
        if (at_f == AT_FDCWD_) {
            // AT_PWD is a path-walk sentinel, never a descriptor to dereference.
            lock(&current->fs->lock);
            fd = current->fs->pwd;
            if (fd != NULL)
                fd_retain(fd);
            unlock(&current->fs->lock);
        } else {
            fd = stat_fd_retain(at_f);
        }
        if (fd == NULL)
            return _EBADF;
        int err = fd->mount->fs->fstat(fd, stat);
        if (info != NULL)
            stat_mount_of_fd(fd, info);
        fd_close(fd);
        return err;
    }

    // An absolute pathname ignores dirfd, even when it is invalid.
    if (path[0] == '/' || at_f == AT_FDCWD_)
        return generic_statat_mount(AT_PWD, path, stat, follow_links, info);
    struct fd *at = stat_fd_retain(at_f);
    if (at == NULL)
        return _EBADF;
    int err = S_ISDIR(at->type) ?
        generic_statat_mount(at, path, stat, follow_links, info) : _ENOTDIR;
    fd_close(at);
    return err;
}

static int stat_path_at(fd_t at_f, const char *path, dword_t flags, struct statbuf *stat) {
    return stat_path_at_mount(at_f, path, flags, stat, NULL);
}

static dword_t sys_stat_path(fd_t at_f, addr_t path_addr,
        addr_t statbuf_addr, dword_t flags) {
    if (flags & ~(AT_SYMLINK_NOFOLLOW_ | AT_EMPTY_PATH_ | AT_NO_AUTOMOUNT_))
        return _EINVAL;
    char path[MAX_PATH];
    // Linux 6.11 also accepts NULL together with AT_EMPTY_PATH.
    if (path_addr == 0 && (flags & AT_EMPTY_PATH_))
        path[0] = '\0';
    else if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("stat(at=%d, path=\"%s\", statbuf=%#llx, flags=%#llx)",
        at_f, path, (unsigned long long)statbuf_addr, (unsigned long long)flags);
    struct statbuf stat = {};
    int err = stat_path_at(at_f, path, flags, &stat);
    if (err < 0)
        return err;
    struct stat_arm64 arm64stat = stat_convert_arm64(stat);
    if (user_put(statbuf_addr, arm64stat))
        return _EFAULT;
    return 0;
}

dword_t sys_stat64(addr_t path_addr, addr_t statbuf_addr) {
    return sys_stat_path(AT_FDCWD_, path_addr, statbuf_addr, 0);
}

dword_t sys_lstat64(addr_t path_addr, addr_t statbuf_addr) {
    return sys_stat_path(AT_FDCWD_, path_addr, statbuf_addr, AT_SYMLINK_NOFOLLOW_);
}

dword_t sys_fstatat64(fd_t at, addr_t path_addr, addr_t statbuf_addr, dword_t flags) {
    return sys_stat_path(at, path_addr, statbuf_addr, flags);
}

dword_t sys_fstat64(fd_t fd_no, addr_t statbuf_addr) {
    STRACE("fstat64(%d, 0x%x)", fd_no, statbuf_addr);
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return _EBADF;
    struct statbuf stat = {};
    int err = fd->mount->fs->fstat(fd, &stat);
    if (err < 0)
        return err;
    struct stat_arm64 arm64stat = stat_convert_arm64(stat);
    if (user_put(statbuf_addr, arm64stat))
        return _EFAULT;
    return 0;
}

dword_t sys_statx(fd_t at_f, addr_t path_addr, int_t flags, uint_t mask, addr_t statx_addr) {
    if ((flags & ~(AT_SYMLINK_NOFOLLOW_ | AT_EMPTY_PATH_ |
            AT_NO_AUTOMOUNT_ | AT_STATX_SYNC_TYPE_)) ||
            (flags & AT_STATX_SYNC_TYPE_) == AT_STATX_SYNC_TYPE_ ||
            (mask & STATX_RESERVED_))
        return _EINVAL;
    char path[MAX_PATH];
    if (path_addr == 0 && (flags & AT_EMPTY_PATH_))
        path[0] = '\0';
    else if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("statx(at=%d, path=\"%s\", flags=%d, mask=%d, statx=%#llx)",
        at_f, path, flags, mask, (unsigned long long)statx_addr);
    struct statbuf stat = {};
    struct stat_mount_info mount_info = {};
    int err = stat_path_at_mount(at_f, path, flags, &stat, &mount_info);
    if (err < 0)
        return err;

    // The basic fields stat returns plus the mount id (Linux 5.8+; required
    // by systemd 256+ path resolution). Like Linux, fields are filled whether
    // or not they were requested; stx_mask says which are valid.
    struct statx_ statx = {};
    statx.mask = STATX_BASIC_STATS_ | STATX_MNT_ID_;
    statx.mnt_id = mount_info.id;
    if (mount_info.known) {
        statx.attributes_mask |= STATX_ATTR_MOUNT_ROOT_;
        if (mount_info.root)
            statx.attributes |= STATX_ATTR_MOUNT_ROOT_;
    }
    statx.blksize = stat.blksize;
    statx.nlink = stat.nlink;
    statx.uid = stat.uid;
    statx.gid = stat.gid;
    statx.mode = stat.mode;
    statx.ino = stat.inode;
    statx.size = stat.size;
    statx.blocks = stat.blocks;
    statx.atime.sec = stat.atime;
    statx.atime.nsec = stat.atime_nsec;
    statx.mtime.sec = stat.mtime;
    statx.mtime.nsec = stat.mtime_nsec;
    statx.ctime.sec = stat.ctime;
    statx.ctime.nsec = stat.ctime_nsec;
    statx.rdev_major = dev_major(stat.rdev);
    statx.rdev_minor = dev_minor(stat.rdev);
    statx.dev_major = dev_major(stat.dev);
    statx.dev_minor = dev_minor(stat.dev);

    if (user_put(statx_addr, statx))
        return _EFAULT;
    return 0;
}
