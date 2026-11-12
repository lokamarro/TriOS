/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RDPID, RDTSCP's ECX and the limit LSL reads from the per-CPU segment
 * (selector 0x7b under Linux) are one value, TSC_AUX: the processor and node
 * the code runs on. The engines have no processor numbering and report
 * processor 0 of node 0, which agrees with itself the same way.
 *
 * The three are read until RDPID gives the same value on both sides of the
 * other two, so a migration between them cannot tell. R8: 1 if RDTSCP's ECX
 * equals RDPID. R9: 1 if LSL's result does.
 */
/* CONFIG
{
  "RegData": {
    "R8": "0x1",
    "R9": "0x1"
  }
}
*/
	.text
	.globl _start
_start:
	movl $64, %r10d
retry:
	.byte 0xf3, 0x41, 0x0f, 0xc7, 0xfb	/* rdpid %r11 */
	.byte 0x0f, 0x01, 0xf9		/* rdtscp */
	movl $0x7b, %edi
	movq $-1, %rsi
	.byte 0x0f, 0x03, 0xf7		/* lsl %di, %esi */
	.byte 0xf3, 0x41, 0x0f, 0xc7, 0xfc	/* rdpid %r12 */
	cmpq %r11, %r12
	je settled
	decl %r10d
	jnz retry
settled:
	xorl %r8d, %r8d
	cmpl %r11d, %ecx
	sete %r8b
	xorl %r9d, %r9d
	cmpl %r11d, %esi
	sete %r9b
	hlt
