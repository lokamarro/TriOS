/* SPDX-License-Identifier: GPL-2.0-or-later */
/* An FMA returns the first of a, b and c of a * b + c that is a NaN,
 * quieted and with its own sign whatever the form negates, and the negative
 * indefinite for a NaN it generates, also beside a quiet NaN addend to an
 * infinity times zero. MXCSR.DAZ flushes its inputs, a scalar form's upper
 * lanes aside, and MXCSR.FTZ a fused result that is tiny after rounding.
 */
/* CONFIG
{
  "RegData": {
    "XMM0": ["0x7fc333337fc11111", "0xffc000007fc55555",
             "0xffc6666640e00000", "0xff8000007fc77777"],
    "XMM3": ["0x7ff8000000011111", "0xfff8000000022222",
             "0x4010000000000000", "0xfff8000000000000"],
    "XMM6": ["0x0000000200000000", "0x004000003f800000"],
    "XMM9": ["0x1111111100000000", "0x0000000322222222"],
    "XMM12": ["0x0080000000000000", "0x40c0000000000000"]
  }
}
*/
	.text
	.globl _start
_start:
	vmovups singles_c(%rip), %ymm0
	vmovups singles_a(%rip), %ymm1
	vmovups singles_b(%rip), %ymm2
	vfmadd231ps %ymm2, %ymm1, %ymm0
	vmovupd doubles_a(%rip), %ymm3
	vmovupd doubles_c(%rip), %ymm4
	vmovupd doubles_b(%rip), %ymm5
	vfnmadd132pd %ymm5, %ymm4, %ymm3
	ldmxcsr denormals_are_zero(%rip)
	vmovups scalar_destination(%rip), %xmm6
	vmovss two(%rip), %xmm7
	vmovss least_times_three(%rip), %xmm8
	vfmsub213ss %xmm8, %xmm7, %xmm6
	ldmxcsr flush_to_zero(%rip)
	vmovups sliver_destination(%rip), %xmm9
	vxorps %xmm10, %xmm10, %xmm10
	vmovss sliver_factor(%rip), %xmm11
	vfmadd132ss %xmm11, %xmm10, %xmm9
	vmovups packed_b(%rip), %xmm12
	vmovups packed_a(%rip), %xmm13
	vmovups packed_c(%rip), %xmm14
	vfmadd213ps %xmm14, %xmm13, %xmm12
	hlt

	.balign 16
denormals_are_zero:
	.long 0x1fc0
flush_to_zero:
	.long 0x9f80
two:
	.long 0x40000000
least_times_three:
	.long 0x00000003
sliver_factor:
	.long 0x00aaaaa5
	.balign 16
singles_a:
	.long 0x7fc11111, 0x3f800000, 0x7f800000, 0x7f800000
	.long 0x40000000, 0xffc66666, 0x3f800000, 0x7f7fffff
singles_b:
	.long 0x7f822222, 0x7fc33333, 0x00000000, 0x00000000
	.long 0x40400000, 0x3f800000, 0x3f800000, 0x40000000
singles_c:
	.long 0x3f800000, 0x7f844444, 0x7fc55555, 0x3f800000
	.long 0x3f800000, 0x00000000, 0x7f877777, 0xff800000
doubles_a:
	.quad 0x7ff0000000011111, 0x3ff0000000000000
	.quad 0x4000000000000000, 0x7ff0000000000000
doubles_b:
	.quad 0x3ff0000000000000, 0xfff8000000022222
	.quad 0x4008000000000000, 0x7ff0000000000000
doubles_c:
	.quad 0x0000000000000000, 0x7ff0000000033333
	.quad 0x4024000000000000, 0x7ff0000000000000
scalar_destination:
	.long 0x00000001, 0x00000002, 0x3f800000, 0x00400000
sliver_destination:
	.long 0x3f400006, 0x11111111, 0x22222222, 0x00000003
packed_a:
	.long 0x3f400006, 0x3f400003, 0x3f400001, 0x40000000
packed_b:
	.long 0x00aaaaa5, 0x00aaaaa8, 0x00aaaaa8, 0x40400000
packed_c:
	.long 0x00000000, 0x00000000, 0x00000001, 0x00000001
