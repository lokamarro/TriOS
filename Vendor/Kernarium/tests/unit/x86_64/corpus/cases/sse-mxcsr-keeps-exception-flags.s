/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MXCSR keeps the exception flags LDMXCSR, FXRSTOR and XRSTOR load, and
 * FXSAVE and XSAVE store them with an MXCSR_MASK of 0xffff.
 *
 * R12 and RBX: STMXCSR after LDMXCSR. RCX and R13: FXSAVE's MXCSR and
 * MXCSR_MASK. RSI: STMXCSR after FXRSTOR. RDI and R8: XSAVE's MXCSR and
 * MXCSR_MASK. R11: STMXCSR after XRSTOR.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x2000"},
  "RegData": {
    "R12": "0x1f81",
    "RBX": "0x1fbf",
    "RCX": "0x1f82",
    "R13": "0xffff",
    "RSI": "0x1f8f",
    "RDI": "0x1fa0",
    "R8": "0xffff",
    "R11": "0x9fd0"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	movl $0x20001000, %r10d
	movl $0x1f80, 512(%r10)

	movl $0x1f81, 512(%r10)
	ldmxcsr 512(%r10)
	stmxcsr 516(%r10)
	movl 516(%r10), %r12d

	movl $0x1fbf, 512(%r10)
	ldmxcsr 512(%r10)
	stmxcsr 516(%r10)
	movl 516(%r10), %ebx

	movl $0x1f82, 512(%r10)
	ldmxcsr 512(%r10)
	fxsave64 (%r9)
	movl 24(%r9), %ecx
	movl 28(%r9), %r13d

	movl $0x1f8f, 24(%r9)
	fxrstor64 (%r9)
	stmxcsr 516(%r10)
	movl 516(%r10), %esi

	movl $0x1fa0, 512(%r10)
	ldmxcsr 512(%r10)
	movl $7, %eax
	xorl %edx, %edx
	xsave64 (%r9)
	movl 24(%r9), %edi
	movl 28(%r9), %r8d

	movl $0x9fd0, 24(%r9)
	movl $7, %eax
	xorl %edx, %edx
	xrstor64 (%r9)
	stmxcsr 516(%r10)
	movl 516(%r10), %r11d

	movl $0x1f80, 512(%r10)
	ldmxcsr 512(%r10)
	hlt
