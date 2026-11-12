/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MXCSR.DAZ makes a denormal input zero for COMIS and UCOMIS, which then
 * compare it equal to zero, for ROUND in any rounding mode, and for
 * VCVTPS2PH, whose rounding up then gives zero.
 */
/* CONFIG
{
  "RegData": {
    "RAX": "0x1",
    "RBX": "0x0",
    "RCX": "0x0",
    "RDX": "0x1",
    "RSI": "0x0",
    "XMM4": ["0x2222222200000000", "0x4444444433333333"],
    "XMM6": ["0x8000000000000000", "0x3ff0000000000000"],
    "XMM8": ["0x8000000000000000", "0x3f8000003f800000",
             "0x4040000000000000", "0x0000000080000000"],
    "XMM10": ["0x3c00000180000000", "0x0000000000000000",
              "0x0000000000000000", "0x0000000000000000"]
  }
}
*/
	.text
	.globl _start
_start:
	ldmxcsr denormals_are_zero(%rip)
	xorl %eax, %eax
	xorl %ebx, %ebx
	xorl %ecx, %ecx
	xorl %edx, %edx
	xorl %esi, %esi
	movss compare_single(%rip), %xmm0
	xorps %xmm1, %xmm1
	ucomiss %xmm1, %xmm0
	setz %al
	setc %bl
	setp %cl
	movsd compare_double(%rip), %xmm2
	xorps %xmm3, %xmm3
	comisd %xmm3, %xmm2
	setz %dl
	setc %sil
	movups round_destination(%rip), %xmm4
	movss round_single(%rip), %xmm5
	roundss $2, %xmm5, %xmm4
	movupd round_doubles(%rip), %xmm7
	roundpd $1, %xmm7, %xmm6
	ldmxcsr denormals_are_zero_up(%rip)
	vmovups round_wide(%rip), %ymm9
	vroundps $4, %ymm9, %ymm8
	movups half_singles(%rip), %xmm11
	vcvtps2ph $2, %xmm11, %xmm10
	hlt

	.balign 16
denormals_are_zero:
	.long 0x1fc0
denormals_are_zero_up:
	.long 0x5fc0
compare_single:
	.long 0x00000001
round_single:
	.long 0x00000001
compare_double:
	.quad 0x8000000000000005
	.balign 16
round_destination:
	.long 0x11111111, 0x22222222, 0x33333333, 0x44444444
round_doubles:
	.quad 0x8000000000000001, 0x3ff8000000000000
round_wide:
	.long 0x00000001, 0x80000001, 0x3f000000, 0x00800000
	.long 0x007fffff, 0x40100000, 0xbf000000, 0x00000000
half_singles:
	.long 0x00000001, 0x80000001, 0x33800000, 0x3f800000
