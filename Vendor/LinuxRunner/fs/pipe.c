#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include "kernel/calls.h"
#include "fs/fd.h"
#include "fs/real.h"
#include "debug.h"

// Native endpoints stay nonblocking. Blocking is a property of each guest
// open-file description, so reopening /proc/self/fd does not alter the source.
static bool pipe_interrupted(void) {
    if (current->group->doing_group_exit) return true;
    lock(&current->sighand->lock);
    bool pending = !!(current->pending & ~current->blocked);
    unlock(&current->sighand->lock);
    return pending;
}
static ssize_t pipe_io(struct fd *fd, void *buf, size_t size, bool writing) {
    unsigned access = __atomic_load_n(&fd->flags, __ATOMIC_RELAXED) & O_ACCMODE_;
    if ((writing && access == O_RDONLY_) || (!writing && access == O_WRONLY_)) return _EBADF;
    if (!size) return 0;
    for (;;) {
        ssize_t result = writing ? write(fd->real_fd, buf, size) : read(fd->real_fd, buf, size);
        if (result >= 0) return result;
        int error = errno;
        if (error == EINTR) {
            if (pipe_interrupted()) return _EINTR;
            continue;
        }
        if (error != EAGAIN && error != EWOULDBLOCK) return errno_map();
        if (__atomic_load_n(&fd->flags, __ATOMIC_RELAXED) & O_NONBLOCK_) return _EAGAIN;
        if (pipe_interrupted()) return _EINTR;
        struct pollfd wait = { .fd = fd->real_fd, .events = writing ? POLLOUT : POLLIN };
        // Match the existing runtime wait cap: a SIGUSR1 arriving between
        // the pending check and host poll entry must not strand group exit.
        // Data still wakes immediately; no new worker or scheduler is used.
        if (poll(&wait, 1, 1000) < 0 && errno != EINTR) return errno_map();
        if (pipe_interrupted()) return _EINTR;
    }
}
static ssize_t pipe_read(struct fd *fd, void *buf, size_t size) { return pipe_io(fd, buf, size, false); }
static ssize_t pipe_write(struct fd *fd, const void *buf, size_t size) { return pipe_io(fd, (void *) buf, size, true); }
static int pipe_getflags(struct fd *fd) { return __atomic_load_n(&fd->flags, __ATOMIC_RELAXED); }
static int pipe_setflags(struct fd *fd, dword_t flags) {
    unsigned old = __atomic_load_n(&fd->flags, __ATOMIC_RELAXED), updated;
    do { updated = (old & ~(O_NONBLOCK_ | O_APPEND_)) | (flags & (O_NONBLOCK_ | O_APPEND_)); }
    while (!__atomic_compare_exchange_n(&fd->flags, &old, updated, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return 0;
}
static int pipe_ioctl(struct fd *fd, int cmd, void *arg) {
    if (cmd == FIONBIO_) return pipe_setflags(fd, (pipe_getflags(fd) & ~O_NONBLOCK_) | (*(int *) arg ? O_NONBLOCK_ : 0));
    return realfs_ioctl(fd, cmd, arg);
}
static const struct fd_ops pipe_fdops;
static struct fd *pipe_reopen(struct fd *source, int flags) {
    // A host pipe cannot synthesize its opposite endpoint. Never fake access.
    if ((flags & O_ACCMODE_) != (pipe_getflags(source) & O_ACCMODE_)) return ERR_PTR(_ENXIO);
    int native = dup(source->real_fd);
    if (native < 0) return ERR_PTR(errno_map());
    struct fd *fd = adhoc_fd_create(&pipe_fdops);
    if (!fd) { close(native); return ERR_PTR(_ENOMEM); }
    fd->real_fd = native;
    fd->stat = source->stat;
    fd->type = S_IFIFO;
    fd->flags = flags & (O_ACCMODE_ | O_NONBLOCK_ | O_APPEND_);
    return fd;
}
static const struct fd_ops pipe_fdops = {
    .read = pipe_read, .write = pipe_write, .poll = realfs_poll,
    .ioctl_size = realfs_ioctl_size, .ioctl = pipe_ioctl, .close = realfs_close,
    .getflags = pipe_getflags, .setflags = pipe_setflags, .reopen = pipe_reopen,
};

static fd_t pipe_f_create(int pipe_fd, int flags, int access, qword_t inode) {
    struct fd *fd = adhoc_fd_create(&pipe_fdops);
    if (fd == NULL) {
        close(pipe_fd);
        return _ENOMEM;
    }
    if (fcntl(pipe_fd, F_SETFL, O_NONBLOCK) < 0) {
        int error = errno_map(); fd->real_fd = pipe_fd; fd_close(fd); return error;
    }
    fd->real_fd = pipe_fd;
    fd->flags = access | (flags & O_NONBLOCK_);
    fd->type = S_IFIFO;
    fd->stat.inode = inode;
    fd->stat.mode = S_IFIFO | 0600;
    fd->stat.uid = current->euid;
    fd->stat.gid = current->egid;
    return f_install(fd, flags);
}

int_t sys_pipe2(addr_t pipe_addr, int_t flags) {
    STRACE("pipe2(%#x, %#x)", pipe_addr, flags);
    if (flags & ~(O_CLOEXEC_|O_NONBLOCK_)) {
        FIXME("unsupported pipe2 flags");
        return _EINVAL;
    }

    int p[2];
    int err = pipe(p);
    if (err < 0)
        return errno_map();

    static atomic_uint_fast64_t next_inode = 1;
    qword_t inode = atomic_fetch_add(&next_inode, 1);
    int fp[2];
    err = fp[0] = pipe_f_create(p[0], flags, O_RDONLY_, inode);
    if (fp[0] < 0) {
        close(p[1]);
        return err;
    }
    err = fp[1] = pipe_f_create(p[1], flags, O_WRONLY_, inode);
    if (fp[1] < 0)
        goto close_fake_0;

    err = _EFAULT;
    if (user_put(pipe_addr, fp))
        goto close_fake_1;
    STRACE(" [%d %d]", fp[0], fp[1]);
    return 0;

close_fake_1:
    f_close(fp[1]);
close_fake_0:
    f_close(fp[0]);
    return err;
}

int_t sys_pipe(addr_t pipe_addr) {
    return sys_pipe2(pipe_addr, 0);
}
