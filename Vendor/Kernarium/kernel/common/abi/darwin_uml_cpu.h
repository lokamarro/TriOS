/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_CPU_ABI_H
#define UML_DARWIN_CPU_ABI_H

#include "uml_darwin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

int darwin_uml_enter_guest(void *registers, void *fp_registers,
			   unsigned long fp_size, unsigned long generation,
			   unsigned long *tls);
unsigned long darwin_uml_guest_hwcap(void);
unsigned long darwin_uml_guest_hwcap2(void);
/* The AT_HWCAP and AT_HWCAP2 words of an AArch32 task (arm64 Linux's
 * COMPAT_HWCAP_* and COMPAT_HWCAP2_* bits) of the run's engine, from bridge
 * ABI 22. Both are zero exactly when the engine does not execute AArch32 EL0
 * for this run: the kernel then refuses 32-bit ELF images and
 * personality(PER_LINUX32), and an AArch32 entry is
 * DARWIN_UML_EVENT_FATAL. The first word always names the architecture's
 * baseline, so it is non-zero exactly when AArch32 runs; no separate query
 * exists. Fixed for the life of the process. */
unsigned long darwin_uml_guest_compat_hwcap(void);
unsigned long darwin_uml_guest_compat_hwcap2(void);
/* Whether the run's engines ignore the top byte of a data address, as EL0
 * does with TCR_EL1.TBI0, from bridge ABI 25: 1 when they do, 0 when they
 * do not. Fixed for the life of the process. An arm64 kernel offers the
 * tagged-address ABI (PR_SET_TAGGED_ADDR_CTRL) only while it is 1, because a
 * tagged pointer faults on a host that keeps the tag. */
unsigned long darwin_uml_guest_top_byte_ignore(void);
/* The streaming vector length in bytes when the virtual CPU implements SME
 * for this run, else 0. Fixed for the life of the process. */
unsigned long darwin_uml_guest_sme_vector_length(void);
/* The virtual CPU's model name, for /proc/cpuinfo. Static storage owned by
 * the emulator, valid for the lifetime of the process, and identical for
 * every guest architecture the host presents. */
const char *darwin_uml_guest_cpu_model_name(void);
int darwin_uml_static_guest_interrupt_sequence(
	const unsigned int *sequence, unsigned int snapshot);

#ifdef __cplusplus
}
#endif

#endif
