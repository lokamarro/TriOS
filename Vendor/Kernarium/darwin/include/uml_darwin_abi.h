/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Canonical Darwin UML host/kernel data ABI.
 *
 * kernel/common/abi/sync.sh copies this file verbatim to the host and to a
 * replayed Linux source.  Change it rather than either copy, and advance
 * kernel/common/abi/identity.env whenever a binary boundary, enum, or layout
 * changes.
 */
#ifndef UML_DARWIN_ABI_H
#define UML_DARWIN_ABI_H

#include "uml_darwin_abi_identity.h"

/* Keep this header usable by both kernel C and Darwin user objects. In
 * particular, do not include <linux/types.h>: Darwin libc headers are already
 * visible in some UML user objects and their POSIX typedefs intentionally
 * differ from the kernel's. These primitive spellings have the required ABI
 * widths on the supported LP64 targets without importing either namespace. */
typedef unsigned char darwin_uml_abi_u8;
typedef unsigned int darwin_uml_abi_u32;
typedef int darwin_uml_abi_s32;
typedef unsigned long long darwin_uml_abi_u64;

#ifndef DARWIN_UML_GUEST_PAGE_SIZE
#define DARWIN_UML_GUEST_PAGE_SIZE 4096
#endif

#ifndef DARWIN_UML_GUEST_PAGE_SHIFT
#define DARWIN_UML_GUEST_PAGE_SHIFT 12
#endif

enum {
	DARWIN_UML_VECTOR_REGISTER_COUNT = 32,
	DARWIN_UML_COMPAT_TLS_COUNT = 3,
};

enum darwin_uml_guest_event {
	DARWIN_UML_EVENT_SYSCALL = 1,
	DARWIN_UML_EVENT_FAULT = 2,
	DARWIN_UML_EVENT_INTERRUPT = 3,
	DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION = 4,
	DARWIN_UML_EVENT_BREAKPOINT = 5,
	/* 6 was the guarded-control-stack fault of the removed FEAT_GCS. */
	DARWIN_UML_EVENT_FLOATING_POINT_TRAP = 7,
	DARWIN_UML_EVENT_PC_ALIGNMENT = 8,
	DARWIN_UML_EVENT_STOP = 9,
	DARWIN_UML_EVENT_DATA_ALIGNMENT = 10,
	/* The task has no SME storage and an instruction needs it: allocate
	 * DARWIN_UML_GUEST_SME_STATE_SIZE(svl) zeroed bytes for the bridge's
	 * streaming vector length, publish them in the task's
	 * struct darwin_uml_guest_sme_registers and re-enter at the same PC,
	 * which retired nothing. */
	DARWIN_UML_EVENT_SME_ACCESS = 11,
	DARWIN_UML_EVENT_FATAL = -1,
};

/* darwin_uml_guest_registers.dispatch_flags: facts about the task that hold
 * for one entry. The kernel sets them before it enters the guest and clears
 * them on return, so they never become part of the task's saved PSTATE. */
enum {
	/* Another agent may observe the memory of the task's mm during this
	 * entry: another task may run on the mm (a vfork child does not count
	 * its waiting parent), the mm maps memory that another mm may map too
	 * (MAP_SHARED of any kind or SysV shm), or io_uring holds pinned
	 * memory of the mm for a ring. The kernel works it out for each entry,
	 * so it clears again once none of these holds. Every store made by
	 * another agent before the condition ended is visible to the entry
	 * that first lacks the flag. A host may rely on this meaning from
	 * bridge ABI 19 in a kernel of the x86-64 userspace slice, the only
	 * one that sets the flag; the arm64 kernels never set it, and the
	 * arm64 host does not read it. */
	DARWIN_UML_DISPATCH_MM_SHARED = 1U << 0,
	/* An AArch32 task may execute the CP15 barrier operations (the MCRs
	 * to DMB, DSB and ISB of the CP15 c7 space). The kernel sets it for
	 * an AArch32 entry while its abi.cp15_barrier setting is not 0:
	 * arm64 Linux emulates the operations by default (1) or lets the
	 * hardware execute them (2), and the host executes them either way.
	 * While it is clear those MCRs are UNDEFINED and raise
	 * DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION. The host never reads it on an
	 * AArch64 entry, and the kernel sets it on no other. From bridge
	 * ABI 22. */
	DARWIN_UML_DISPATCH_A32_CP15BEN = 1U << 1,
};

enum {
	DARWIN_UML_FAULT_WRITE = 2,
	DARWIN_UML_FAULT_EXECUTE = 16,
	DARWIN_UML_FAULT_PAGE = 14,
};

/* ESR-shaped metadata carried in fault.error_code for an FP trap. */
#define DARWIN_UML_FLOATING_POINT_EC (0x2cU << 26)
#define DARWIN_UML_FLOATING_POINT_IL (1U << 25)
#define DARWIN_UML_FLOATING_POINT_TFV (1U << 23)
#define DARWIN_UML_FLOATING_POINT_INVALID (1U << 0)
#define DARWIN_UML_FLOATING_POINT_DIVIDE_BY_ZERO (1U << 1)
#define DARWIN_UML_FLOATING_POINT_OVERFLOW (1U << 2)
#define DARWIN_UML_FLOATING_POINT_UNDERFLOW (1U << 3)
#define DARWIN_UML_FLOATING_POINT_INEXACT (1U << 4)
#define DARWIN_UML_FLOATING_POINT_INPUT_DENORMAL (1U << 7)
#define DARWIN_UML_FLOATING_POINT_SYNDROME_BASE \
	(DARWIN_UML_FLOATING_POINT_EC | DARWIN_UML_FLOATING_POINT_IL | \
	 DARWIN_UML_FLOATING_POINT_TFV)

#ifndef DARWIN_UML_SIGRETURN_TRAMPOLINE
#define DARWIN_UML_SIGRETURN_TRAMPOLINE 0x3fffffffff00ULL
#endif
#define DARWIN_UML_NR_RT_SIGRETURN 139ULL

struct darwin_uml_faultinfo {
	darwin_uml_abi_s32 error_code;
	darwin_uml_abi_s32 padding;
	darwin_uml_abi_u64 address;
	darwin_uml_abi_s32 trap_number;
	darwin_uml_abi_s32 trailing_padding;
};

/* Prefix passed unchanged between a UML task and the native host runtime.
 *
 * An AArch32 task of an arm64 kernel is identified by PSTATE.nRW (pstate
 * bit 4) set, and exists from bridge ABI 22 on. The same fields mean:
 *   pstate  the SPSR layout of an exception taken from AArch32: M[4:0] =
 *           0b10000 (User), T (bit 5), E (bit 9, always 0), GE (19:16),
 *           IT[1:0] (26:25), IT[7:2] (15:10), Q (27), NZCV (31:28), SS
 *           (21) and DIT (24); A, I, F, IL and PAN are 0.
 *   pc      the architectural address of the next instruction, bit 0
 *           clear; T is in pstate.
 *   x[0-14] R0-R14 in bits 31:0 (SP_usr is x13, LR_usr is x14). The host
 *           writes bits 63:32 as zero on return and ignores them on
 *           entry. x[15-28], fp, lr and sp are no part of the state, and
 *           the host leaves them as they were.
 *   fp registers  D0-D31 in vectors[0-15], D(2n) in bits 63:0 of V(n) and
 *           D(2n+1) in bits 127:64; vectors[16-31] are ignored on entry and
 *           unchanged on return. fpsr holds FPSCR & 0xf800009f and fpcr
 *           FPSCR & 0x07f79f00; FPSCR.Len and .Stride read as zero and the
 *           floating-point trap enables read as zero and ignore writes.
 *   tls     (the tls argument of darwin_uml_enter_guest()) TPIDRURW in
 *           bits 31:0, read on entry and written on return.
 *   tpidrro TPIDRURO, read on entry only.
 * An AArch32 entry whose svcr is non-zero, or that the run's engine cannot
 * execute (darwin_uml_guest_compat_hwcap() is zero), is
 * DARWIN_UML_EVENT_FATAL. The events mean, for such a task:
 *   SYSCALL   pc is after the SVC (4 bytes in A32, 2 or 4 in T32) and the
 *             IT state in pstate has advanced past it; the kernel takes
 *             the system call number from x7 and ignores the immediate.
 *   ILLEGAL_INSTRUCTION  pc and pstate (IT included) are those of the
 *             instruction, which has done nothing.
 *   BREAKPOINT  a BKPT instruction at pc, or a completed single step with
 *             fault.trap_number 0x32 and pc at the next instruction.
 *   FAULT, DATA_ALIGNMENT, PC_ALIGNMENT  as for AArch64, with 32-bit
 *             addresses. */
struct darwin_uml_guest_registers {
	darwin_uml_abi_u64 x[29];
	/* For a task of an x86-64 userspace kernel, the address of its mm's
	 * LDT, or 0 when the mm has none: 8192 descriptors in x86 format,
	 * zero where no entry is installed. The kernel changes a descriptor
	 * only with one aligned 8-byte store and frees the table only with
	 * the mm. A host may rely on this from bridge ABI 20. */
	darwin_uml_abi_u64 fp;
	darwin_uml_abi_u64 lr;
	darwin_uml_abi_u64 sp;
	darwin_uml_abi_u64 pc;
	darwin_uml_abi_u32 pstate;
	darwin_uml_abi_u32 dispatch_flags;
	darwin_uml_abi_u64 original_x0;
	struct darwin_uml_faultinfo fault;
	long long syscall;
	darwin_uml_abi_s32 is_user;
	darwin_uml_abi_s32 padding;
	darwin_uml_abi_u64 fpmr;
	/* An arm64 task's PSTATE.SM in bit 0 and PSTATE.ZA in bit 1; the other
	 * bits stay zero. */
	darwin_uml_abi_u64 svcr;
	/* The i386 TLS descriptors of a task of an x86_64-userspace kernel,
	 * whichever ABI installed them, one struct user_desc each; zero for
	 * every other task. */
	darwin_uml_abi_u64 compat_tls[DARWIN_UML_COMPAT_TLS_COUNT][2];
	/* TPIDRURO of an AArch32 task of an arm64 kernel, in bits 31:0: the
	 * kernel sets it for the ARM-private set_tls call, CLONE_SETTLS and a
	 * tracer's write, and the host reads it on entry and never writes it,
	 * because EL0 cannot. Zero for every other task. From bridge ABI 22. */
	darwin_uml_abi_u64 tpidrro;
};

struct darwin_uml_guest_fp_registers {
	darwin_uml_abi_u8 vectors[DARWIN_UML_VECTOR_REGISTER_COUNT][16];
	darwin_uml_abi_u32 fpsr;
	darwin_uml_abi_u32 fpcr;
	darwin_uml_abi_u32 reserved[2];
};

/* Every arm64 task's register tail holds this immediately after its
 * struct darwin_uml_guest_fp_registers, whether or not the run implements
 * SME; an x86_64-userspace task's tail ends after the FP registers. */
struct darwin_uml_guest_sme_registers {
	darwin_uml_abi_u64 tpidr2;   /* TPIDR2_EL0; 0 without SME */
	darwin_uml_abi_u64 state;    /* address of the task's SME storage, or 0 */
	darwin_uml_abi_u32 svl;      /* bytes per vector the storage holds, or 0 */
	darwin_uml_abi_u32 reserved; /* 0 */
};

/* A task's SME storage, DARWIN_UML_GUEST_SME_STATE_SIZE(svl) bytes for a
 * streaming vector length of svl bytes: Z0-Z31 of svl bytes, P0-P15 of
 * svl / 8 bytes, ZA as its svl horizontal vectors of svl bytes, then the
 * 64-byte ZT0, each part at a multiple of svl. Bytes 0-15 of each stored Z
 * are a copy of the matching V in struct darwin_uml_guest_fp_registers,
 * which owns them: the host ignores them on entry and writes them on exit. */
#define DARWIN_UML_GUEST_SME_Z_OFFSET(svl, n) ((n) * (svl))
#define DARWIN_UML_GUEST_SME_P_OFFSET(svl, n) (32 * (svl) + (n) * ((svl) / 8))
#define DARWIN_UML_GUEST_SME_ZA_OFFSET(svl, row) (34 * (svl) + (row) * (svl))
#define DARWIN_UML_GUEST_SME_ZT0_OFFSET(svl) (34 * (svl) + (svl) * (svl))
#define DARWIN_UML_GUEST_SME_ZT0_SIZE 64
#define DARWIN_UML_GUEST_SME_STATE_SIZE(svl) \
	(DARWIN_UML_GUEST_SME_ZT0_OFFSET(svl) + DARWIN_UML_GUEST_SME_ZT0_SIZE)

/* Layout constants make the native and kernel-side checks independent of a
 * compiler's spelling of the corresponding structures. */
#define DARWIN_UML_ABI_GUEST_REGISTERS_DISPATCH_FLAGS_OFFSET 268U
#define DARWIN_UML_ABI_GUEST_REGISTERS_ORIGINAL_X0_OFFSET 272U
#define DARWIN_UML_ABI_GUEST_REGISTERS_FAULT_OFFSET 280U
#define DARWIN_UML_ABI_GUEST_REGISTERS_SYSCALL_OFFSET 304U
#define DARWIN_UML_ABI_GUEST_REGISTERS_FPMR_OFFSET 320U
#define DARWIN_UML_ABI_GUEST_REGISTERS_COMPAT_TLS_OFFSET 336U
#define DARWIN_UML_ABI_GUEST_REGISTERS_TPIDRRO_OFFSET 384U
#define DARWIN_UML_ABI_GUEST_REGISTERS_SIZE 392U
#define DARWIN_UML_ABI_GUEST_FP_REGISTERS_SIZE 528U
#define DARWIN_UML_ABI_GUEST_SME_REGISTERS_OFFSET 528U
#define DARWIN_UML_ABI_GUEST_SME_REGISTERS_SIZE 24U
/* The fp_size an arm64 task passes to darwin_uml_enter_guest(). */
#define DARWIN_UML_ABI_GUEST_ARM64_TAIL_SIZE 552U

#if defined(__cplusplus)
#include <stddef.h>
static_assert(DARWIN_UML_EVENT_FLOATING_POINT_TRAP == 7);
static_assert(DARWIN_UML_EVENT_PC_ALIGNMENT == 8);
static_assert(DARWIN_UML_EVENT_STOP == 9);
static_assert(DARWIN_UML_EVENT_DATA_ALIGNMENT == 10);
static_assert(DARWIN_UML_EVENT_SME_ACCESS == 11);
static_assert(DARWIN_UML_FLOATING_POINT_SYNDROME_BASE == 0xb2800000U);
static_assert(DARWIN_UML_GUEST_PAGE_SIZE ==
		(1ULL << DARWIN_UML_GUEST_PAGE_SHIFT));
static_assert(DARWIN_UML_GUEST_PAGE_SIZE == 4096);
static_assert(offsetof(struct darwin_uml_guest_registers, dispatch_flags) ==
		DARWIN_UML_ABI_GUEST_REGISTERS_DISPATCH_FLAGS_OFFSET);
static_assert(offsetof(struct darwin_uml_guest_registers, original_x0) ==
		DARWIN_UML_ABI_GUEST_REGISTERS_ORIGINAL_X0_OFFSET);
static_assert(offsetof(struct darwin_uml_guest_registers, fault) ==
		DARWIN_UML_ABI_GUEST_REGISTERS_FAULT_OFFSET);
static_assert(offsetof(struct darwin_uml_guest_registers, syscall) ==
		DARWIN_UML_ABI_GUEST_REGISTERS_SYSCALL_OFFSET);
static_assert(offsetof(struct darwin_uml_guest_registers, fpmr) ==
		DARWIN_UML_ABI_GUEST_REGISTERS_FPMR_OFFSET);
static_assert(offsetof(struct darwin_uml_guest_registers, compat_tls) ==
		DARWIN_UML_ABI_GUEST_REGISTERS_COMPAT_TLS_OFFSET);
static_assert(offsetof(struct darwin_uml_guest_registers, tpidrro) ==
		DARWIN_UML_ABI_GUEST_REGISTERS_TPIDRRO_OFFSET);
static_assert(sizeof(struct darwin_uml_guest_registers) ==
		DARWIN_UML_ABI_GUEST_REGISTERS_SIZE);
static_assert(sizeof(struct darwin_uml_faultinfo) == 24U);
static_assert(sizeof(struct darwin_uml_guest_fp_registers) ==
		DARWIN_UML_ABI_GUEST_FP_REGISTERS_SIZE);
static_assert(sizeof(struct darwin_uml_guest_sme_registers) ==
		DARWIN_UML_ABI_GUEST_SME_REGISTERS_SIZE);
static_assert(DARWIN_UML_ABI_GUEST_SME_REGISTERS_OFFSET ==
		DARWIN_UML_ABI_GUEST_FP_REGISTERS_SIZE);
static_assert(DARWIN_UML_ABI_GUEST_ARM64_TAIL_SIZE ==
		DARWIN_UML_ABI_GUEST_SME_REGISTERS_OFFSET +
		DARWIN_UML_ABI_GUEST_SME_REGISTERS_SIZE);
static_assert(DARWIN_UML_GUEST_SME_STATE_SIZE(64) == 6336);
#endif

#endif
