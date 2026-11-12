// SPDX-License-Identifier: GPL-2.0-or-later

	.text
	.p2align	2
	.globl	_uml_arm64_jit_chain_abi_probe
_uml_arm64_jit_chain_abi_probe:
	stp	x29, x30, [sp, #-64]!
	mov	x29, sp
	stp	x19, x20, [sp, #16]
	stp	x21, x22, [sp, #32]
	stp	x23, x24, [sp, #48]
	mov	x24, x1
	mov	x19, #0x1919
	mov	x20, #0x2020
	mov	x21, #0x2121
	mov	x22, #0x2222
	mov	x23, #0x2323
	bl	_uml_arm64_jit_chain_abi_callback
	stp	x19, x20, [x24]
	stp	x21, x22, [x24, #16]
	str	x23, [x24, #32]
	ldp	x23, x24, [sp, #48]
	ldp	x21, x22, [sp, #32]
	ldp	x19, x20, [sp, #16]
	ldp	x29, x30, [sp], #64
	ret
