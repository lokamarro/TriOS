// SPDX-License-Identifier: GPL-2.0-or-later
/* The entry point of the AArch32 engine probe: the initial stack the kernel
 * builds (argc, argv, envp, auxv) goes to probe_main(). */
	.syntax	unified
	.arm
	.text
	.globl	_start
	.type	_start, %function
_start:
	mov	r0, sp
	bic	sp, sp, #7
	bl	probe_main
	udf	#0
