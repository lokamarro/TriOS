#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

REPOSITORY=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$REPOSITORY/kernel/mainline-6.18"
COMPAT_ROOT="$LINE_ROOT/build/toolchain/darwin-host"
COMPAT_INCLUDE="$COMPAT_ROOT/include"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-darwin-host-compat.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test_darwin_host_compat.sh
. "$(dirname -- "$0")/../support/expect.sh"

HOST_CC=$(command -v cc) || fail "cc is unavailable"

# ELF declarations come from the pinned kernel-host-tools prefix, not from a
# repository wrapper that could shadow the real elfutils headers.
for header in elf.h libelf.h; do
	[ ! -e "$COMPAT_INCLUDE/$header" ] ||
		fail "the compatibility directory shadows <$header>"
done

cat >"$TEST_ROOT/compat.c" <<'EOF'
#include <asm/errno.h>
#include <byteswap.h>
#include <endian.h>
#include <errno.h>
#include <stdio_ext.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/vfs.h>

#ifndef UML_DARWIN_HOST_BYTESWAP_H
#error "the repository byteswap.h wrapper was not selected"
#endif
#ifndef UML_DARWIN_HOST_ENDIAN_H
#error "the repository endian.h wrapper was not selected"
#endif
#ifndef UML_DARWIN_HOST_ASM_ERRNO_H
#error "the repository asm/errno.h wrapper was not selected"
#endif
#if _FORTIFY_SOURCE != 0
#error "host tools must not inherit Darwin's default fortification"
#endif
#ifdef strlcpy
#error "strlcpy must remain definable by Linux host-tool sources"
#endif

/* tools/include defines __cold as empty; the compatibility layer must too. */
#define __cold

_Static_assert(bswap_16(0x1234) == 0x3412, "16-bit byte swap changed");
_Static_assert(bswap_32(0x12345678U) == 0x78563412U,
	"32-bit byte swap changed");
_Static_assert(bswap_64(0x0123456789abcdefULL) == 0xefcdab8967452301ULL,
	"64-bit byte swap changed");
_Static_assert(__BYTE_ORDER == __LITTLE_ENDIAN, "host byte order changed");
_Static_assert(EAGAIN == 35, "Darwin errno values were overridden");
_Static_assert(EUCLEAN != 0 && EBADFD != 0, "Linux-only errno names missing");
_Static_assert(AF_NETLINK != AF_UNIX && AF_PACKET != AF_UNIX,
	"Linux address families collide with Darwin families");

int main(void)
{
	struct epoll_event event = { .events = EPOLLIN };
	struct statfs filesystem;

	if (htobe32(0x12345678U) != 0x78563412U || le16toh(0x1234) != 0x1234)
		return 1;
	errno = 0;
	if (syscall(__NR_bpf, 0, 0, 0) != -1 || errno != ENOSYS)
		return 2;
	errno = 0;
	if (epoll_create1(EPOLL_CLOEXEC) != -1 || errno != ENOSYS ||
	    epoll_ctl(0, EPOLL_CTL_ADD, 0, &event) != -1)
		return 3;
	if (__fsetlocking(stdout, FSETLOCKING_BYCALLER) != FSETLOCKING_INTERNAL)
		return 4;
	if (statfs("/", &filesystem) != 0)
		return 5;
	return 0;
}
EOF

"$HOST_CC" -std=gnu11 -Wall -Wextra -Werror \
	-include "$COMPAT_ROOT/host_compat.h" -I"$COMPAT_INCLUDE" \
	"$TEST_ROOT/compat.c" -o "$TEST_ROOT/compat"
"$TEST_ROOT/compat" || fail "compatibility runtime check failed with $?"

printf '%s\n' "Darwin host compatibility header tests passed"
