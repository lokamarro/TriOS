/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_HOST_ASM_ERRNO_H
#define UML_DARWIN_HOST_ASM_ERRNO_H

/*
 * Linux host tools include <asm/errno.h> through their own linux/err.h.
 * They run against Darwin's C library, so its errno values must win over the
 * Linux UAPI numbering. Only the Linux-specific names Darwin lacks are added.
 */
#include <errno.h>

#ifndef EBADFD
#define EBADFD 77
#endif
#ifndef EUCLEAN
#define EUCLEAN 117
#endif

#endif /* UML_DARWIN_HOST_ASM_ERRNO_H */
