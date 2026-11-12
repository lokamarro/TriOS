/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MXCSR.FTZ flushes a result that is tiny after rounding: one that, rounded
 * to full precision as if the exponent were unbounded, is below the smallest
 * normal, though rounding to the format gives the smallest normal itself. A
 * result that rounds to the smallest normal from no lower is kept. Products
 * reach that sliver to nearest; quotients only in a directed mode.
 */
/* CONFIG
{
  "RegData": {
    "XMM0": ["0x0000000000000000", "0x0000000000000000"],
    "XMM1": ["0x0000000000800000", "0x0000000000000000"],
    "XMM2": ["0x0080000000000000", "0x0000000080000000"],
    "XMM4": ["0x0000000000000000", "0x0000000000000000"],
    "XMM5": ["0x0000000000000000", "0x0010000000000000",
             "0x8000000000000000", "0x4008000000000000"],
    "XMM6": ["0x0000000000000000", "0x0000000000000000"],
    "XMM7": ["0x0000000000000000", "0x3f00000000000000",
             "0x4000000080000000", "0x008000003eaaaaab"]
  }
}
*/
	.text
	.globl _start
_start:
	ldmxcsr flush_nearest(%rip)
	movss product_sliver(%rip), %xmm0
	mulss product_sliver+4(%rip), %xmm0
	movss product_normal(%rip), %xmm1
	mulss product_normal+4(%rip), %xmm1
	movups packed_left(%rip), %xmm2
	movups packed_right(%rip), %xmm3
	mulps %xmm3, %xmm2
	movsd double_sliver(%rip), %xmm4
	mulsd double_sliver+8(%rip), %xmm4
	vmovupd wide_doubles_left(%rip), %ymm8
	vmulpd wide_doubles_right(%rip), %ymm8, %ymm5
	ldmxcsr flush_up(%rip)
	movss quotient_sliver(%rip), %xmm6
	divss quotient_sliver+4(%rip), %xmm6
	vmovups wide_dividends(%rip), %ymm9
	vdivps wide_divisors(%rip), %ymm9, %ymm7
	hlt

	.balign 16
flush_nearest:
	.long 0x9f80
flush_up:
	.long 0xdf80
product_sliver:
	.long 0x3f400006, 0x00aaaaa5
product_normal:
	.long 0x3f400003, 0x00aaaaa8
quotient_sliver:
	.long 0x00a97359, 0x3fa9735a
	.balign 16
packed_left:
	.long 0x3f400006, 0x3f400003, 0xbf400006, 0x3f800000
packed_right:
	.long 0x00aaaaa5, 0x00aaaaa8, 0x00aaaaa5, 0x00400000
double_sliver:
	.quad 0x3fea6a3a6513270e, 0x00136209b26a539f
wide_doubles_left:
	.quad 0x3fea6a3a6513270e, 0x3fe892f9d23f0824
	.quad 0xbfea6a3a6513270e, 0x3ff8000000000000
wide_doubles_right:
	.quad 0x00136209b26a539f, 0x0014d5bd89925faa
	.quad 0x00136209b26a539f, 0x4000000000000000
wide_dividends:
	.long 0x00a97359, 0x00f953a5, 0x00934f05, 0x3f800000
	.long 0x80a97359, 0x40c00000, 0x3f800000, 0x00800000
wide_divisors:
	.long 0x3fa9735a, 0x3ff953a6, 0x3f934f06, 0x40000000
	.long 0x3fa9735a, 0x40400000, 0x40400000, 0x3f800000
