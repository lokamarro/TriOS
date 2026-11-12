/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_HOST_STDIO_EXT_H
#define UML_DARWIN_HOST_STDIO_EXT_H

/*
 * glibc stream-locking controls used by elfutils' Linux process readers.
 * Darwin streams always lock internally, so a request is only reported back.
 */
#include <stdio.h>

#define FSETLOCKING_QUERY 0
#define FSETLOCKING_INTERNAL 1
#define FSETLOCKING_BYCALLER 2

static inline int __fsetlocking(FILE *stream, int type)
{
	(void)stream;
	(void)type;
	return FSETLOCKING_INTERNAL;
}

#endif /* UML_DARWIN_HOST_STDIO_EXT_H */
