/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Loading a signaling NaN single or double onto the x87 stack raises
 * invalid operation and quiets it; an 80-bit load does not.
 *
 * R10: FLD of a single SNaN. RBX: FLD of a double SNaN. RCX: FADD of a
 * single SNaN. RDX: FLD of an 80-bit SNaN. RSI: the double FLD pushed.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "R10": "0x3801",
    "RBX": "0x3801",
    "RCX": "0x3801",
    "RDX": "0x3800",
    "RSI": "0x7ffc000000000000"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	movl $0x7fa00000, (%r9)
	movabsq $0x7ff4000000000000, %r10
	movq %r10, 8(%r9)
	movabsq $0xa000000000000000, %r10
	movq %r10, 16(%r9)
	movw $0x7fff, 24(%r9)

	fninit
	flds (%r9)
	fnstsw %ax
	movzwl %ax, %r10d

	fninit
	fldl 8(%r9)
	fnstsw %ax
	movzwl %ax, %ebx
	fstpl 32(%r9)
	movq 32(%r9), %rsi

	fninit
	fld1
	fadds (%r9)
	fnstsw %ax
	movzwl %ax, %ecx

	fninit
	fldt 16(%r9)
	fnstsw %ax
	movzwl %ax, %edx
	fninit
	hlt
