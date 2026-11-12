#include <sys/stat.h>
#include "kernel/task.h"
#include "kernel/calls.h"
#include "fs/poll.h"

static lock_t epoll_graph_lock = LOCK_INITIALIZER;
static struct list epoll_graph = LIST_INITIALIZER(epoll_graph);
// Cache DAG heights within a serialized graph check; shared descendants must
// not turn a modest set of descriptors into an exponential graph walk.
static uint64_t epoll_epoch;
static int epoll_graph_height(struct poll *node, struct poll *parent, struct poll *child,
                              uint64_t epoch, unsigned depth) {
    if (depth >= 5) return _ELOOP;
    if (node->epoll_epoch == epoch)
        return node->epoll_height ? (int)node->epoll_height : _ELOOP;
    node->epoll_epoch = epoch; node->epoll_height = 0;
    int height = 1, error = 0; struct poll_fd *pf;
    lock(&node->lock);
    list_for_each_entry(&node->poll_fds, pf, fds) {
        if (pf->fd->ops == &epoll_fdops) {
            int n = epoll_graph_height(pf->fd->epollfd.poll,parent,child,epoch,depth+1);
            if (n < 0) { error = n; break; }
            if (height < n+1) height = n+1;
        }
    }
    if (!error && node == parent) {
        int n = epoll_graph_height(child,parent,child,epoch,depth+1);
        if (n < 0) error = n;
        else if (height < n+1) height = n+1;
    }
    unlock(&node->lock);
    if (error) return error;
    node->epoll_height = height;
    return height;
}
static int epoll_graph_check(struct poll *parent, struct poll *child) {
    struct poll *node;
    if (++epoll_epoch == 0) {
        list_for_each_entry(&epoll_graph,node,epoll_link) node->epoll_epoch = 0;
        epoll_epoch = 1;
    }
    list_for_each_entry(&epoll_graph, node, epoll_link) {
        int height = epoll_graph_height(node,parent,child,epoll_epoch,0);
        if (height < 0 || height > 5) return _ELOOP;
    }
    return 0;
}

fd_t sys_epoll_create(int_t flags) {
    STRACE("epoll_create(%#x)", flags);
    if (flags & ~(O_CLOEXEC_))
        return _EINVAL;

    struct fd *fd = adhoc_fd_create(&epoll_fdops);
    if (fd == NULL)
        return _ENOMEM;
    struct poll *poll = poll_create();
    if (IS_ERR(poll)) { fd_close(fd); return PTR_ERR(poll); }
    fd->epollfd.poll = poll;
    fd->flags = O_RDWR_;
    fd->real_fd = poll->real.fd; // pollable native kqueue/epoll backend
    lock(&epoll_graph_lock); list_add(&epoll_graph,&poll->epoll_link); unlock(&epoll_graph_lock);
    return f_install(fd, flags);
}
fd_t sys_epoll_create0() {
    return sys_epoll_create(0);
}

struct epoll_event_ {
    uint32_t events;
#ifdef GUEST_ARM64
    uint32_t __pad;
#endif
    uint64_t data;
}
#ifndef GUEST_ARM64
__attribute__((packed))
#endif
;

#define EPOLL_CTL_ADD_ 1
#define EPOLL_CTL_DEL_ 2
#define EPOLL_CTL_MOD_ 3
#define EPOLLET_ (1 << 31)
#define EPOLLONESHOT_ (1 << 30)

static int epoll_ctl_description(struct fd *,struct fd *,int_t,fd_t,addr_t);
int_t sys_epoll_ctl(fd_t epoll_f, int_t op, fd_t f, addr_t event_addr) {
    STRACE("epoll_ctl(%d, %d, %d, %#x)", epoll_f, op, f, event_addr);
    struct fd *epoll = f_get_retain(epoll_f);
    if (!epoll) return _EBADF;
    struct fd *fd = f_get_retain(f);
    if (!fd) { fd_close(epoll); return _EBADF; }
    int result;
    if (epoll->ops != &epoll_fdops) result = _EINVAL;
    else {
        bool nested = fd->ops == &epoll_fdops;
        if (nested) lock(&epoll_graph_lock);
        result = epoll_ctl_description(epoll,fd,op,f,event_addr);
        if (nested) unlock(&epoll_graph_lock);
    }
    fd_close(fd); fd_close(epoll); return result;
}
static int epoll_ctl_description(struct fd *epoll, struct fd *fd, int_t op, fd_t f, addr_t event_addr) {

    // Regular files (and directories) are not pollable: they are always
    // "ready", so Linux rejects adding them to an epoll set with EPERM. iSH
    // used to accept them, which makes bun's fs.WriteStream take its polling
    // path (instead of the EPERM fallback) and then throw EEXIST on the second
    // registration of the same fd. Match Linux and reject with EPERM.
    if (op == EPOLL_CTL_ADD_ && (S_ISREG(fd->type) || S_ISDIR(fd->type)))
        return _EPERM;

    if (op != EPOLL_CTL_ADD_ && op != EPOLL_CTL_DEL_ && op != EPOLL_CTL_MOD_)
        return _EINVAL;
    if (fd == epoll) return _EINVAL;
    if (op == EPOLL_CTL_DEL_)
        return poll_del_fd(epoll->epollfd.poll, fd, f);

    struct epoll_event_ event;
    if (user_get(event_addr, event))
        return _EFAULT;
    STRACE(" {events: %#x, data: %#x}", event.events, event.data);

    if (op == EPOLL_CTL_ADD_) {
        if (poll_has_fd(epoll->epollfd.poll, fd, f)) return _EEXIST;
        if (fd->ops == &epoll_fdops) {
            int error = epoll_graph_check(epoll->epollfd.poll,fd->epollfd.poll);
            if (error) return error;
        }
        return poll_add_fd(epoll->epollfd.poll, fd, f, event.events, (union poll_fd_info) event.data);
    } else {
        return poll_mod_fd(epoll->epollfd.poll, fd, f, event.events, (union poll_fd_info) event.data);
    }
}

struct epoll_context {
    struct epoll_event_ *events;
    int n;
    int max_events;
};

static int epoll_callback(void *context, int types, union poll_fd_info info) {
    struct epoll_context *c = context;
    if (c->n >= c->max_events)
        return 0;
    c->events[c->n++] = (struct epoll_event_) {.events = types, .data = info.num};
    return 1;
}

static int epoll_wait_description(struct fd *epoll, addr_t events_addr, int_t max_events, int_t timeout) {
    if (epoll->ops != &epoll_fdops)
        return _EINVAL;

    struct timespec timeout_ts;
    if (timeout >= 0) {
        timeout_ts.tv_sec = timeout / 1000;
        timeout_ts.tv_nsec = (timeout % 1000) * 1000000;
    }
    if (max_events <= 0 || max_events > INT32_MAX / sizeof(struct epoll_event_))
        return _EINVAL;
    // Linux permits returning fewer than maxevents. Never put a guest-sized
    // VLA on the host thread stack or allocate attacker-selected RAM.
    struct epoll_event_ events[128];
    struct epoll_context context = {.events = events, .n = 0,
        .max_events = max_events < 128 ? max_events : 128};
    STRACE("...\n");
    int res = poll_wait(epoll->epollfd.poll, epoll_callback, &context, timeout < 0 ? NULL : &timeout_ts);
    STRACE("%d end epoll_wait", current->pid);
    if (res >= 0) {
        for (int i = 0; i < res; i++) {
            STRACE(" {events: %#x, data: %#x}", events[i].events, events[i].data);
        }
        if (user_write(events_addr, events, sizeof(struct epoll_event_) * res))
            return _EFAULT;
    }
    return res;
}

int_t sys_epoll_wait(fd_t epoll_f, addr_t events_addr, int_t max_events, int_t timeout) {
    struct fd *epoll = f_get_retain(epoll_f);
    if (!epoll) return _EBADF;
    int result = epoll_wait_description(epoll,events_addr,max_events,timeout);
    fd_close(epoll); return result;
}

int_t sys_epoll_pwait(fd_t epoll_f, addr_t events_addr, int_t max_events, int_t timeout, addr_t sigmask_addr, dword_t sigsetsize) {
    sigset_t_ mask;
    if (sigmask_addr != 0) {
        int err = user_get_sigset(sigmask_addr, sigsetsize, &mask);
        if (err)
            return err;
        sigmask_set_temp(mask);
    }

    return sys_epoll_wait(epoll_f, events_addr, max_events, timeout);
}

static int epoll_close(struct fd *fd) {
    if (fd->epollfd.poll) {
        lock(&epoll_graph_lock);
        list_remove(&fd->epollfd.poll->epoll_link);
        poll_destroy(fd->epollfd.poll);
        unlock(&epoll_graph_lock);
    }
    return 0;
}

static int epoll_poll(struct fd *fd) { return poll_has_ready(fd->epollfd.poll) ? POLL_READ : 0; }
const struct fd_ops epoll_fdops = {
    .poll = epoll_poll,
    .close = epoll_close,
};
