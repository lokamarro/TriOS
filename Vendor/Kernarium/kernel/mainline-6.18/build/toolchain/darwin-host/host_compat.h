/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/*
 * Compatibility injected only into Linux host tools on Darwin. Keeping these
 * definitions outside the Linux source avoids carrying build-host policy in
 * the maintained kernel delta. dependencies/build-kernel-host-tools.sh also
 * injects it into the pinned libbpf that pahole embeds.
 */
#ifndef __linux__
/*
 * Darwin fortifies string functions unless a translation unit opts out, and
 * its fortify macros rewrite definitions such as the strlcpy fallback in
 * tools/lib/string.c. glibc fortification is opt-in, so match that default;
 * a tool that requests _FORTIFY_SOURCE explicitly still gets it.
 */
#ifndef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 0
#endif

#include <errno.h>
#include <sys/cdefs.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
/*
 * os/base.h tests __has_attribute(fallthrough), which no longer parses once
 * tools/include defines a fallthrough macro. Include it before any tool
 * header can.
 */
#include <os/base.h>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

/* tools/include defines __cold as empty; replace the SDK attribute with the
 * same empty definition so the two do not conflict. */
#undef __cold
#define __cold

/* Unexported UAPI headers spell the const attribute with glibc's macro. */
#ifndef __attribute_const__
#define __attribute_const__ __attribute__((__const__))
#endif

static inline ssize_t uml_darwin_copy_file_range(
	int input, off_t *input_offset, int output, off_t *output_offset,
	size_t length, unsigned int flags)
{
	(void)input;
	(void)input_offset;
	(void)output;
	(void)output_offset;
	(void)length;
	(void)flags;
	errno = ENOSYS;
	return -1;
}

#define copy_file_range uml_darwin_copy_file_range

/*
 * libbpf reaches Linux system calls only when a tool drives a running Linux
 * kernel, which host tools on Darwin never do. Fail those calls with ENOSYS
 * rather than passing Linux system-call numbers to Darwin.
 */
static inline long uml_darwin_linux_syscall(long number, ...)
{
	(void)number;
	errno = ENOSYS;
	return -1;
}

#define syscall uml_darwin_linux_syscall

#ifndef __NR_dup3
#define __NR_dup3 24
#endif
#ifndef __NR_perf_event_open
#define __NR_perf_event_open 241
#endif
#ifndef __NR_memfd_create
#define __NR_memfd_create 279
#endif
#ifndef __NR_bpf
#define __NR_bpf 280
#endif

/* Address families Darwin lacks; sockets for them fail with EAFNOSUPPORT. */
#ifndef AF_NETLINK
#define AF_NETLINK 255
#endif
#ifndef AF_PACKET
#define AF_PACKET 254
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#ifdef __clang__
/*
 * Darwin deprecates sprintf, and libbpf sizes one stack array with a const
 * variable that Clang folds as a GNU extension. Both are accepted by the
 * Linux toolchains these sources target.
 */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wgnu-folding-constant"
#endif
#endif
