/* SPDX-License-Identifier: GPL-2.0-or-later */
/* System transitions, architectural identification, counters, and fault exit. */
gadget syscall
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_stream]
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	str x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	str x9, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 11 * 8)]
	strb wzr, [_vcpu, #UML_X86_64_CPU_SYSTEM_CALL_32BIT]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_SYSCALL
	b uml_x86_64_gadget_leave

gadget int80
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_stream]
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	mov w9, #1
	strb w9, [_vcpu, #UML_X86_64_CPU_SYSTEM_CALL_32BIT]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_SYSCALL
	b uml_x86_64_gadget_leave

gadget breakpoint
	ldr x8, [_stream]
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_BREAKPOINT
	b uml_x86_64_gadget_leave

/* ICEBP (INT1) raises #DB as a trap: it retires, and RIP names the next
 * instruction. */
gadget icebp
	ldr x8, [_stream]
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_DEBUG_TRAP
	b uml_x86_64_gadget_leave

gadget cpuid
	ldr w5, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	ldr w10, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	mov w6, wzr
	mov w7, wzr
	mov w8, wzr
	mov w9, wzr
	cbz w5, Lx86_cpuid_zero
	cmp w5, #1
	b.eq Lx86_cpuid_one
	cmp w5, #7
	b.eq Lx86_cpuid_seven
	cmp w5, #13
	b.eq Lx86_cpuid_extended_state
	mov w11, #0x8000
	lsl w11, w11, #16
	cmp w5, w11
	b.eq Lx86_cpuid_extended_zero
	add w12, w11, #1
	cmp w5, w12
	b.eq Lx86_cpuid_extended_one
	add w12, w11, #2
	cmp w5, w12
	b.eq Lx86_cpuid_brand_two
	add w12, w11, #3
	cmp w5, w12
	b.eq Lx86_cpuid_brand_three
	b Lx86_cpuid_done
Lx86_cpuid_zero:
	mov w6, #13
	mov w7, #0x6547
	movk w7, #0x756e, lsl #16
	mov w9, #0x6e69
	movk w9, #0x4965, lsl #16
	mov w8, #0x746e
	movk w8, #0x6c65, lsl #16
	b Lx86_cpuid_done
Lx86_cpuid_one:
	mov w6, #0x06c3
	movk w6, #0x0003, lsl #16
	mov w7, #0x0800
	/* SSE3, PCLMUL, SSSE3, FMA, CMPXCHG16B, SSE4.1/4.2, MOVBE,
	 * POPCNT, AES, XSAVE/OSXSAVE, AVX, F16C, and RDRAND.
	 */
	mov w8, #0x3203
	movk w8, #0x7ed8, lsl #16
	mov w9, #0x8111
	/* x87, TSC, CMPXCHG8B, CMOV, CLFLUSH, MMX, FXSR, SSE, SSE2. */
	movk w9, #0x0788, lsl #16
	b Lx86_cpuid_done
Lx86_cpuid_seven:
	cbz w10, Lx86_cpuid_seven_zero
	cmp w10, #1
	b.ne Lx86_cpuid_done
	/* Subleaf 1: AVX-VNNI, the VEX VPDPBUSD, VPDPBUSDS, VPDPWSSD and
	 * VPDPWSSDS. */
	mov w6, #0x0010
	b Lx86_cpuid_done
Lx86_cpuid_seven_zero:
	/* The highest subleaf, 1 for AVX-VNNI's. */
	mov w6, #1
	/* FSGSBASE, BMI1, AVX2, BMI2, ERMS, RDSEED, ADX, CLFLUSHOPT,
	 * CLWB, and SHA. RTM/HLE and MPX remain deliberately absent.
	 * ERMS claims REP MOVSB is the fastest way to copy, which holds here:
	 * a repeated string operation runs page-bounded block moves, and no
	 * vector routine a resolver would pick instead comes close.
	 */
	mov w7, #0x0329
	movk w7, #0x218c, lsl #16
	/* RDPID. */
	mov w8, wzr
	movk w8, #0x0040, lsl #16
	/* FSRM. ERMS says REP MOVSB is the fastest copy; FSRM says so at the
	 * short sizes where per-instruction setup dominates, and a guest probe
	 * moving 256 MB in 256-byte calls has REP MOVSB ahead of both the AVX2
	 * and SSE2 routines a resolver would pick instead.
	 */
	mov w9, #0x0010
	b Lx86_cpuid_done
Lx86_cpuid_extended_state:
	cbz w10, Lx86_cpuid_extended_state_zero
	cmp w10, #2
	b.ne Lx86_cpuid_done
	/* AVX contributes 256 bytes of YMM_Hi128 state at offset 576. */
	mov w6, #256
	mov w7, #576
	b Lx86_cpuid_done
Lx86_cpuid_extended_state_zero:
	/* XCR0 enables x87, SSE, and AVX.  The standard XSAVE image is 832
	 * bytes for both the enabled and complete supported feature sets.
	 */
	mov w6, #7
	mov w7, #832
	mov w8, #832
	b Lx86_cpuid_done
Lx86_cpuid_extended_zero:
	mov w6, #0x8000
	lsl w6, w6, #16
	add w6, w6, #8
	b Lx86_cpuid_done
Lx86_cpuid_extended_one:
	mov w8, #0x20
	mov w9, #0x0800
	movk w9, #0x2010, lsl #16
	b Lx86_cpuid_done
Lx86_cpuid_brand_two:
	/* The brand string is plain little-endian bytes, so the first
	 * word is 'K','e','r','n' from the low byte up. Registers
	 * were zeroed at entry, so a word that is entirely brand
	 * padding is simply left alone. */
	/* "Kern" */
	mov w6, #0x654b
	movk w6, #0x6e72, lsl #16
	/* "ariu" */
	mov w7, #0x7261
	movk w7, #0x7569, lsl #16
	/* "m Vi" */
	mov w8, #0x206d
	movk w8, #0x6956, lsl #16
	/* "rtua" */
	mov w9, #0x7472
	movk w9, #0x6175, lsl #16
	b Lx86_cpuid_done
Lx86_cpuid_brand_three:
	/* "l CP" */
	mov w6, #0x206c
	movk w6, #0x5043, lsl #16
	/* "U\0\0\0" */
	mov w7, #0x0055
Lx86_cpuid_done:
	str x6, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	str x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	str x9, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	str x7, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 3 * 8)]
	/* CPUID serializes, so it is a code-change checkpoint: a vCPU that saw
	 * the flag of a cross-modifying-code protocol, then executes it, must
	 * run the code the writer left. The run leaves for the instruction
	 * after it, and the runtime revalidates what it selects there. */
	ldr x8, [_stream]
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave

gadget xgetbv
	ldr x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	cmp x8, #0
	mov w9, #7
	csel w8, w9, wzr, eq
	str x8, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	str xzr, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	retire_and_next

.macro timestamp rdtscp
gadget \rdtscp
	ldr x23, [_stream], #8
	ldrb w8, [_vcpu, #UML_X86_64_CPU_TIMESTAMP_FAULTS]
	cbnz w8, Lx86_general_protection
	isb
	mrs x8, cntvct_el0
	mov w9, w8
	str x9, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	lsr x8, x8, #32
	str x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	.ifc \rdtscp,rdtscp
	str xzr, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	.endif
	retire_and_next
.endm

timestamp rdtsc
timestamp rdtscp
.purgem timestamp

gadget privileged
	ldr x23, [_stream]
/* A faulting instruction does not retire; RIP names the instruction, which
 * the stream word in x23 locates. */
Lx86_general_protection:
	instruction_rip x22
	str x22, [_vcpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_GENERAL_PROTECTION
	b uml_x86_64_gadget_leave

/* RDPID reads TSC_AUX, which is zero here as RDTSCP's ECX and the per-CPU
 * segment's limit are: a guest vCPU has no processor numbering, so every
 * vCPU reports processor 0 of node 0. */
gadget rdpid_32_r
	ldr x8, [_stream], #8
	str xzr, [_vcpu, x8]
	retire_and_next

gadget rdpid_64_r
	ldr x8, [_stream], #8
	str xzr, [_vcpu, x8]
	retire_and_next

.macro read_segment_base name, base, host_register
gadget read_\name\()_base_\host_register\()_r
	ldr x8, [_stream], #8
	ldr x9, [_vcpu, #\base]
	.ifc \host_register,32
	mov w9, w9
	.endif
	str x9, [_vcpu, x8]
	retire_and_next
.endm

/* A 64-bit base must be canonical: bits 63:47 all equal, or #GP. */
.macro write_segment_base name, base, width, host_register
gadget write_\name\()_base_\width\()_r
	ldr x8, [_stream], #8
	.if \width == 64
	ldr x23, [_stream], #8
	.endif
	ldr \host_register\()9, [_vcpu, x8]
	.if \width == 64
	sbfx x10, x9, #0, #48
	cmp x10, x9
	b.ne Lx86_general_protection
	.endif
	str x9, [_vcpu, #\base]
	retire_and_next
.endm

read_segment_base fs, UML_X86_64_CPU_FS_BASE, 32
read_segment_base fs, UML_X86_64_CPU_FS_BASE, 64
read_segment_base gs, UML_X86_64_CPU_GS_BASE, 32
read_segment_base gs, UML_X86_64_CPU_GS_BASE, 64
write_segment_base fs, UML_X86_64_CPU_FS_BASE, 32, w
write_segment_base fs, UML_X86_64_CPU_FS_BASE, 64, x
write_segment_base gs, UML_X86_64_CPU_GS_BASE, 32, w
write_segment_base gs, UML_X86_64_CPU_GS_BASE, 64, x

.purgem write_segment_base
.purgem read_segment_base

gadget memory_fence
	dmb ish
	retire_and_next

/* CLFLUSH, CLFLUSHOPT and CLWB read the line they name as far as paging goes:
 * an operand on an unmapped or inaccessible page raises #PF for a read at the
 * operand's own address. The host cache needs no flushing. */
gadget cache_flush
	effective_address
	translate 0, UML_X86_64_TLB_READ_PAGE, 1
	dmb ish
	retire_and_next

.macro store_machine_status_register width, store, host_register
gadget store_machine_status_\width\()_r
	ldr x8, [_stream], #8
	mov w9, #0x0033
	movk w9, #0x8005, lsl #16
	\store \host_register\()9, [_vcpu, x8]
	retire_and_next
.endm

store_machine_status_register 16, strh, w
store_machine_status_register 32, str, w
store_machine_status_register 64, str, x

.purgem store_machine_status_register

gadget store_machine_status_16_m
	effective_address
	mov x25, x7
	mov w24, #0x0033
	movk w24, #0x8005, lsl #16
	page_cross_check 2, Lx86_store_machine_status_cross
	translate 1, UML_X86_64_TLB_WRITE_PAGE, 2
	strh w24, [x7]
Lx86_store_machine_status_done:
	finish_store 2
Lx86_store_machine_status_cross:
	cross_page_store x24, 2
	b Lx86_store_machine_status_done

gadget mxcsr_load
	effective_address
	mov x25, x7
	page_cross_check 4, Lx86_mxcsr_load_cross
	translate 0, UML_X86_64_TLB_READ_PAGE, 4
	ldr w24, [x7]
Lx86_mxcsr_load_done:
	/* A value with a reserved bit, one of 31:16, raises #GP(0) and loads
	 * nothing. MXCSR holds what the guest loads, masks and flags included,
	 * as STMXCSR shows. The loaded flags replace the host's. */
	tst w24, #0xffff0000
	b.ne Lx86_general_protection
	str w24, [_vcpu, #UML_X86_64_CPU_MXCSR]
	msr fpsr, xzr
	refresh_simd_exact w24, 9, 10
	publish_guest_rounding 9, 10, 11
	retire_unless_simd_unchecked w24
Lx86_mxcsr_load_cross:
	cross_page_load 4
	mov w24, w0
	b Lx86_mxcsr_load_done

gadget mxcsr_store
	effective_address
	mov x25, x7
	ldr w24, [_vcpu, #UML_X86_64_CPU_MXCSR]
	fold_host_flags w24, 9, 10
	str w24, [_vcpu, #UML_X86_64_CPU_MXCSR]
	page_cross_check 4, Lx86_mxcsr_store_cross
	translate 1, UML_X86_64_TLB_WRITE_PAGE, 4
	str w24, [x7]
Lx86_mxcsr_store_done:
	finish_store 4
Lx86_mxcsr_store_cross:
	cross_page_store x24, 4
	b Lx86_mxcsr_store_done

.p2align 4
Lx86_memory_fault:
	instruction_rip x22
/* The fault of a gadget that has the instruction's address in x22. */
Lx86_memory_fault_at:
	ldrb w8, [_context, #UML_X86_64_CONTEXT_SPLIT_LOCK]
	cbz w8, Lx86_memory_fault_unlocked
	split_lock_leave
Lx86_memory_fault_unlocked:
	str x22, [_vcpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_MEMORY_FAULT
	b uml_x86_64_gadget_leave

/* The instruction at x23 wrote into the block that is running: it retires,
 * and the run leaves for the instruction after it, whose code may have
 * changed. Every memory gadget that stores shares this exit. */
Lx86_store_code_exit:
	instruction_rip x8, 1
	str x8, [_vcpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_barrier
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave

/* A repeated string instruction spent its repeat budget and stops between
 * elements with RIP still naming it. */
Lx86_repeat_yield:
	instruction_rip x22
	str x22, [_vcpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_REPEAT_YIELD
	b uml_x86_64_gadget_leave

/* An access through a segment failed its inline check, or a guard stopped
 * an instruction the engine cannot check: it leaves, as a fault would,
 * before anything committed, with the stream word in x23 naming it, for the
 * runtime's exact check (`prepare_segment_step`). */
Lx86_segment_check:
	instruction_rip x22
	ldrb w8, [_context, #UML_X86_64_CONTEXT_SPLIT_LOCK]
	cbz w8, Lx86_segment_check_unlocked
	split_lock_leave
Lx86_segment_check_unlocked:
	str x22, [_vcpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_SEGMENT_CHECK
	b uml_x86_64_gadget_leave

/* Ahead of an instruction whose accesses through a segment the engine does
 * not check inline: it leaves for the exact check unless the runtime runs the
 * instruction under a bypass, having checked it. */
gadget segment_guard
	ldr x23, [_stream], #8
	ldrb w8, [_vcpu, #UML_X86_64_CPU_SEGMENT_BYPASS]
	cbz w8, Lx86_segment_check
	next_gadget
