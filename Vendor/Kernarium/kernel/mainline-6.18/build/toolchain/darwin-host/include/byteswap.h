/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_HOST_BYTESWAP_H
#define UML_DARWIN_HOST_BYTESWAP_H

/* Match the helpers consumed by unmodified Linux host-tool sources. */
#define bswap_16(value) __builtin_bswap16(value)
#define bswap_32(value) __builtin_bswap32(value)
#define bswap_64(value) __builtin_bswap64(value)

#endif /* UML_DARWIN_HOST_BYTESWAP_H */
