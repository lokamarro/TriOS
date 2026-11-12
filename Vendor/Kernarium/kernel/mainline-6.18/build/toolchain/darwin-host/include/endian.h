/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_HOST_ENDIAN_H
#define UML_DARWIN_HOST_ENDIAN_H

/* Provide the glibc <endian.h> names consumed by Linux host-tool sources. */
#include <machine/endian.h>

#ifndef __BYTE_ORDER
#define __BYTE_ORDER BYTE_ORDER
#define __LITTLE_ENDIAN LITTLE_ENDIAN
#define __BIG_ENDIAN BIG_ENDIAN
#endif

#if BYTE_ORDER == LITTLE_ENDIAN
#define htobe16(value) __builtin_bswap16(value)
#define htole16(value) ((__uint16_t)(value))
#define be16toh(value) __builtin_bswap16(value)
#define le16toh(value) ((__uint16_t)(value))
#define htobe32(value) __builtin_bswap32(value)
#define htole32(value) ((__uint32_t)(value))
#define be32toh(value) __builtin_bswap32(value)
#define le32toh(value) ((__uint32_t)(value))
#define htobe64(value) __builtin_bswap64(value)
#define htole64(value) ((__uint64_t)(value))
#define be64toh(value) __builtin_bswap64(value)
#define le64toh(value) ((__uint64_t)(value))
#else
#error "Darwin host tools are built only on little-endian hosts"
#endif

#endif /* UML_DARWIN_HOST_ENDIAN_H */
