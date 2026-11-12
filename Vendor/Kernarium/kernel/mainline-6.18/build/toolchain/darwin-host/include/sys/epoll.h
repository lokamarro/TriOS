/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_HOST_SYS_EPOLL_H
#define UML_DARWIN_HOST_SYS_EPOLL_H

/*
 * libbpf's ring-buffer consumer uses epoll only for maps of a running Linux
 * kernel. Host tools on Darwin never create one, so the calls fail with
 * ENOSYS.
 */
#include <errno.h>
#include <stdint.h>

#define EPOLLIN 0x001
#define EPOLLOUT 0x004
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLL_CLOEXEC 02000000

typedef union epoll_data {
	void *ptr;
	int fd;
	uint32_t u32;
	uint64_t u64;
} epoll_data_t;

struct epoll_event {
	uint32_t events;
	epoll_data_t data;
};

static inline int epoll_create1(int flags)
{
	(void)flags;
	errno = ENOSYS;
	return -1;
}

static inline int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
	(void)epfd;
	(void)op;
	(void)fd;
	(void)event;
	errno = ENOSYS;
	return -1;
}

static inline int epoll_wait(int epfd, struct epoll_event *events,
	int maxevents, int timeout)
{
	(void)epfd;
	(void)events;
	(void)maxevents;
	(void)timeout;
	errno = ENOSYS;
	return -1;
}

#endif /* UML_DARWIN_HOST_SYS_EPOLL_H */
