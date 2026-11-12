/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The legacy encodings of CMPPS, CMPPD, CMPSS and CMPSD read the low three
 * bits of their immediate, as x86 hardware does, where VEX reads five: 8 is
 * equal and ordered, 0x0d not less than, 0x0b unordered, 0x0c not equal and
 * 0xff ordered, while VEX's 8 is equal or unordered.
 */
/* CONFIG
{
  "RegData": {
    "XMM0": ["0xffffffff00000000", "0x0000000000000000"],
    "XMM2": ["0xffffffffffffffff", "0xffffffffffffffff"],
    "XMM4": ["0xffffffffffffffff", "0x0000000000000000"],
    "XMM6": ["0x3f800000ffffffff", "0x3f80000040000000"],
    "XMM8": ["0x0000000000000000", "0x3ff0000000000000"],
    "XMM9": ["0xffffffffffffffff", "0xffffffff00000000"]
  }
}
*/
	.text
	.globl _start
_start:
	movups left_single(%rip), %xmm0
	movups right_single(%rip), %xmm1
	movaps %xmm0, %xmm2
	movaps %xmm0, %xmm6
	movaps %xmm0, %xmm10
	movupd left_double(%rip), %xmm4
	movupd right_double(%rip), %xmm5
	movapd %xmm4, %xmm8
	cmpps $0x08, %xmm1, %xmm0
	cmpps $0x0d, %xmm1, %xmm2
	cmppd $0x0b, %xmm5, %xmm4
	cmpss $0x0c, %xmm1, %xmm6
	cmpsd $0xff, %xmm5, %xmm8
	vcmpps $0x08, %xmm1, %xmm10, %xmm9
	hlt

	.balign 16
left_single:
	.long 0x7fc00000, 0x3f800000, 0x40000000, 0x3f800000
right_single:
	.long 0x3f800000, 0x3f800000, 0x3f800000, 0x7fc00000
left_double:
	.quad 0x7ff8000000000000, 0x3ff0000000000000
right_double:
	.quad 0x3ff0000000000000, 0x4000000000000000
