#include <string.h>
#include <sys/stat.h>
#include "debug.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/real.h"
#include "kernel/errno.h"

static struct mount adhoc_mount;
#undef adhoc_fd_create
struct fd *adhoc_fd_create(const struct fd_ops *ops) { return adhoc_fd_create_at(ops,"external"); }

struct fd *adhoc_fd_create_at(const struct fd_ops *ops, const char *site) {
    struct fd *fd = fd_create_at(ops,site);
    if (fd == NULL)
        return NULL;
    mount_retain(&adhoc_mount);
    fd->mount = &adhoc_mount;
    fd->stat = (struct statbuf) {};
    return fd;
}

static int adhoc_fstat(struct fd *fd, struct statbuf *stat) {
    *stat = fd->stat;
    if (fd->memfd) {
        lock(&fd->memfd->lock);
        *stat = fd->memfd->stat;
        unlock(&fd->memfd->lock);
    }
    // Anonymous regular backing files (memfd) need live size/block data.
    // Pipes and other anonymous descriptors retain their synthetic metadata.
    if (fd->ops == &realfs_fdops && S_ISREG(fd->stat.mode)) {
        struct stat backing;
        if (fstat(fd->real_fd, &backing) < 0) return errno_map();
        stat->size = backing.st_size;
        stat->blocks = backing.st_blocks;
        stat->blksize = backing.st_blksize;
        stat->inode = backing.st_ino;
    }
    return 0;
}

static int adhoc_fsetattr(struct fd *fd, struct attr attr) {
    if (fd->memfd) {
        if (attr.type == attr_size) return realfs_fsetattr(fd, attr);
        lock(&fd->memfd->lock);
        if (attr.type == attr_uid) fd->memfd->stat.uid = attr.uid;
        else if (attr.type == attr_gid) fd->memfd->stat.gid = attr.gid;
        else if (attr.type == attr_mode) fd->memfd->stat.mode = (fd->memfd->stat.mode & S_IFMT) | (attr.mode & ~S_IFMT);
        unlock(&fd->memfd->lock);
        return 0;
    }
    switch (attr.type) {
        case attr_uid:
            fd->stat.uid = attr.uid;
            break;
        case attr_gid:
            fd->stat.gid = attr.gid;
            break;
        case attr_mode:
            fd->stat.mode = (fd->stat.mode & S_IFMT) | (attr.mode & ~S_IFMT);
            break;
        case attr_size:
            if (fd->ops == &realfs_fdops && S_ISREG(fd->stat.mode))
                return realfs_fsetattr(fd, attr);
            return _EINVAL;
    }
    return 0;
}

static int adhoc_getpath(struct fd *fd, char *buf) {
    if (fd->memfd) {
        snprintf(buf, MAX_PATH, "/memfd:%s (deleted)", fd->memfd->name);
        return 0;
    }
    const char *type = fd->ops == &epoll_fdops ? "eventpoll" : S_ISFIFO(fd->stat.mode) ? "pipe" : S_ISSOCK(fd->stat.mode) ? "socket" : "unknown"; // TODO allow this to be customized
    if (fd->stat.inode == 0)
        sprintf(buf, "anon_inode:[%s]", type);
    else
        sprintf(buf, "%s:[%lu]", type, (unsigned long) fd->stat.inode);
    return 0;
}

bool is_adhoc_fd(struct fd *fd) {
    return fd->mount == &adhoc_mount;
}

static const struct fs_ops adhoc_fs = {
    .magic = 0x09041934, // FIXME wrong for pipes and sockets
    .fstat = adhoc_fstat,
    .fsetattr = adhoc_fsetattr,
    .getpath = adhoc_getpath,
};

static struct mount adhoc_mount = {
    .fs = &adhoc_fs,
    .point = "",
};
