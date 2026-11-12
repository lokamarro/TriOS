/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MXCSR.DAZ makes a conversion's denormal input zero: widened, narrowed, or
 * rounded up or down to an integer. MXCSR.FTZ flushes a narrowed single that
 * is tiny after rounding, though rounding up gives the smallest normal.
 */
/* CONFIG
{
  "RegData": {
    "RAX": "0x0",
    "RBX": "0x0",
    "RCX": "0x0000000100000000",
    "XMM0": ["0x0000000000000000", "0x3333333344444444"],
    "XMM2": ["0x8000000000000000", "0x3ff0000000000000"],
    "XMM4": ["0x0000000000000000", "0x7ff8000020000000",
             "0xc000000000000000", "0x8000000000000000"],
    "XMM6": ["0x6666666600000000", "0x8888888877777777"],
    "XMM8": ["0x0080000000000000", "0x8000000000000000",
             "0x0000000000000000", "0x0000000000000000"],
    "XMM10": ["0x0000000100000000", "0xffffffff00000000"]
  }
}
*/
	.text
	.globl _start
_start:
	ldmxcsr flush_up(%rip)
	movups widen_destination(%rip), %xmm0
	movss least(%rip), %xmm1
	cvtss2sd %xmm1, %xmm0
	movups widen_singles(%rip), %xmm3
	cvtps2pd %xmm3, %xmm2
	movups wide_singles(%rip), %xmm5
	vcvtps2pd %xmm5, %ymm4
	movups narrow_destination(%rip), %xmm6
	movsd narrow_sliver(%rip), %xmm7
	cvtsd2ss %xmm7, %xmm6
	vmovupd narrow_doubles(%rip), %ymm9
	vcvtpd2ps %ymm9, %xmm8
	movups integer_singles(%rip), %xmm11
	cvtps2dq %xmm11, %xmm10
	ldmxcsr zero_down(%rip)
	movq $0x1234, %rax
	movq $0x5678, %rbx
	movsd negative_least_double(%rip), %xmm12
	cvtsd2si %xmm12, %rax
	movss negative_three_least(%rip), %xmm13
	cvtss2si %xmm13, %ebx
	movsd pair_singles(%rip), %xmm14
	cvtps2pi %xmm14, %mm0
	movq %mm0, %rcx
	emms
	hlt

	.balign 16
flush_up:
	.long 0xdfc0
zero_down:
	.long 0x3fc0
least:
	.long 0x00000001
negative_three_least:
	.long 0x80000003
negative_least_double:
	.quad 0x8000000000000001
narrow_sliver:
	.quad 0x380fffffdfffcfc7
pair_singles:
	.long 0x80000002, 0x3fc00000
	.balign 16
widen_destination:
	.quad 0x1111111122222222, 0x3333333344444444
widen_singles:
	.long 0x80000005, 0x3f800000, 0x00000000, 0x00000000
wide_singles:
	.long 0x00400000, 0x7f800001, 0xc0000000, 0x807fffff
narrow_destination:
	.long 0x55555555, 0x66666666, 0x77777777, 0x88888888
narrow_doubles:
	.quad 0x380fffffdfffcfc7, 0x380ffffffffffc18
	.quad 0x0000000000000001, 0xb80fffffdfffcfc7
integer_singles:
	.long 0x00000001, 0x3f000001, 0x80000001, 0xbfc00000
