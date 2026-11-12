/* SPDX-License-Identifier: GPL-2.0-or-later */
/* x87 instructions that round nothing clear C1, which a quotient that
 * rounded up set before each, as on the Xeon 8581C: the loads, a store to
 * a register, FCHS, FABS, FXCH, the compares and the stack-pointer
 * instructions, and an exact result.
 *
 * Each row loads 1, then one third, which sets C1, runs one instruction
 * and saves the status word; the registers hold four rows' status words
 * each, the first row in the low word of RAX, in this order:
 *  0 fld %st(0)
 *  1 fld1
 *  2 fldz
 *  3 fldpi
 *  4 fldl2e
 *  5 flds
 *  6 fldt
 *  7 filds
 *  8 fst st1
 *  9 fstp st1
 * 10 fsts, which rounds one third up
 * 11 fchs
 * 12 fabs
 * 13 fxch
 * 14 fcom
 * 15 ftst
 * 16 fincstp
 * 17 fdecstp
 * 18 ffree
 * 19 fnop
 * 20 fucomi
 * 21 fcmovb
 * 22 fldcw
 * 23 fnstenv
 * 24 frndint exact
 * 25 fsqrt exact
 * 26 fadd exact
 * 27 fistp exact
 * 28 fcomi
 * 29 fcomip
 * 30 fucomip
 * 31 fcomp
 * 32 fucom
 * 33 fucompp
 * 34 fstpt
 * 35 fnclex
 * 36 ficoml
 * 37 ffreep
 * 38 fstp alias DF D0+i
 * 39 fxch alias DD C8+i
 * 40 fistpl of one third
 * 41 fisttpl of one third
 * 42 fxtract
 * 43 fscale
 * 44 fprem
 * 45 fxam
 * 46 fcom alias DC D0+i
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x2820282028202820",
    "RBX": "0x2820282028202820",
    "RCX": "0x3020322038203020",
    "RDX": "0x3020312030203020",
    "RSI": "0x3220302028203820",
    "RDI": "0x3220322032203220",
    "RBP": "0x3020302030203020",
    "R8": "0x39203a203a203220",
    "R10": "0x3200382001203120",
    "R11": "0x3020382038203120",
    "R12": "0x3020282038203820",
    "R13": "0x312034203020"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	fninit
	fnstcw 0x50(%r9)
	/* fld %st(0) */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fld %st(0)
	fnstsw 0x100(%r9)
	/* fld1 */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fld1
	fnstsw 0x102(%r9)
	/* fldz */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fldz
	fnstsw 0x104(%r9)
	/* fldpi */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fldpi
	fnstsw 0x106(%r9)
	/* fldl2e */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fldl2e
	fnstsw 0x108(%r9)
	/* flds */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	flds one32(%rip)
	fnstsw 0x10a(%r9)
	/* fldt */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fldt one80(%rip)
	fnstsw 0x10c(%r9)
	/* filds */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fildl int32(%rip)
	fnstsw 0x10e(%r9)
	/* fst st1 */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fst %st(1)
	fnstsw 0x110(%r9)
	/* fstp st1 */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fstp %st(1)
	fnstsw 0x112(%r9)
	/* fsts exact */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fsts 0x40(%r9)
	fnstsw 0x114(%r9)
	/* fchs */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fchs
	fnstsw 0x116(%r9)
	/* fabs */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fabs
	fnstsw 0x118(%r9)
	/* fxch */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fxch %st(1)
	fnstsw 0x11a(%r9)
	/* fcom */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fcom %st(1)
	fnstsw 0x11c(%r9)
	/* ftst */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	ftst
	fnstsw 0x11e(%r9)
	/* fincstp */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fincstp
	fnstsw 0x120(%r9)
	/* fdecstp */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fdecstp
	fnstsw 0x122(%r9)
	/* ffree */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	ffree %st(2)
	fnstsw 0x124(%r9)
	/* fnop */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fnop
	fnstsw 0x126(%r9)
	/* fucomi */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fucomi %st(1), %st
	fnstsw 0x128(%r9)
	/* fcmovb */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fcmovb %st(1), %st
	fnstsw 0x12a(%r9)
	/* fldcw */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fldcw 0x50(%r9)
	fnstsw 0x12c(%r9)
	/* fnstenv */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fnstenv 0x60(%r9)
	fnstsw 0x12e(%r9)
	/* frndint exact */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fstp %st(0)
	fld1
	frndint
	fnstsw 0x130(%r9)
	/* fsqrt exact */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fstp %st(0)
	fld1
	fsqrt
	fnstsw 0x132(%r9)
	/* fadd exact */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fstp %st(0)
	fld1
	fadd %st(0), %st
	fnstsw 0x134(%r9)
	/* fistp exact */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fstp %st(0)
	fld1
	fistpl 0x48(%r9)
	fld1
	fnstsw 0x136(%r9)
	/* fcomi */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fcomi %st(1), %st
	fnstsw 0x138(%r9)
	/* fcomip */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fcomip %st(1), %st
	fnstsw 0x13a(%r9)
	/* fucomip */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fucomip %st(1), %st
	fnstsw 0x13c(%r9)
	/* fcomp */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fcomp %st(1)
	fnstsw 0x13e(%r9)
	/* fucom */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fucom %st(1)
	fnstsw 0x140(%r9)
	/* fucompp */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fucompp
	fnstsw 0x142(%r9)
	/* fstpt */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fstpt 0x70(%r9)
	fnstsw 0x144(%r9)
	/* fnclex */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fnclex
	fnstsw 0x146(%r9)
	/* ficoml */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	ficoml int32(%rip)
	fnstsw 0x148(%r9)
	/* ffreep */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	ffreep %st(1)
	fnstsw 0x14a(%r9)
	/* fstp alias DF D0+i */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	.byte 0xdf, 0xd1
	fnstsw 0x14c(%r9)
	/* fxch alias DD C8+i */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	.byte 0xdd, 0xc9
	fnstsw 0x14e(%r9)
	/* fistpl of one third */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fistpl 0x48(%r9)
	fnstsw 0x150(%r9)
	/* fisttpl of one third */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fisttpl 0x48(%r9)
	fnstsw 0x152(%r9)
	/* fxtract */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fxtract
	fnstsw 0x154(%r9)
	/* fscale */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fscale
	fnstsw 0x156(%r9)
	/* fprem */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fprem
	fnstsw 0x158(%r9)
	/* fxam */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	fxam
	fnstsw 0x15a(%r9)
	/* fcom alias DC D0+i */
	fninit
	fld1
	fld1
	fdivl three(%rip)
	.byte 0xdc, 0xd1
	fnstsw 0x15c(%r9)
	movq 0x100(%r9), %rax
	movq 0x108(%r9), %rbx
	movq 0x110(%r9), %rcx
	movq 0x118(%r9), %rdx
	movq 0x120(%r9), %rsi
	movq 0x128(%r9), %rdi
	movq 0x130(%r9), %rbp
	movq 0x138(%r9), %r8
	movq 0x140(%r9), %r10
	movq 0x148(%r9), %r11
	movq 0x150(%r9), %r12
	movq 0x158(%r9), %r13
	fninit
	xorl %r9d, %r9d
	hlt
	.p2align 3
three:
	.double 3.0
one32:
	.float 1.0
	.p2align 4
one80:
	.quad 0x8000000000000000
	.short 0x3fff
	.p2align 2
int32:
	.long 7
