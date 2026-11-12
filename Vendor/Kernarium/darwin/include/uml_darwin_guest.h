/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_GUEST_H
#define UML_DARWIN_GUEST_H

#include "uml_darwin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Concrete engine entry points are host-internal implementation seams. The
 * Linux guest calls only darwin_uml_enter_guest from the synchronized ABI. */
int darwin_uml_jit_enter_guest(void *registers, void *fp_registers,
	unsigned long fp_size, unsigned long generation, unsigned long *tls);
int darwin_uml_static_enter_guest(void *registers, void *fp_registers,
	unsigned long fp_size, unsigned long generation, unsigned long *tls);

#ifdef __cplusplus
}
#endif

#endif
