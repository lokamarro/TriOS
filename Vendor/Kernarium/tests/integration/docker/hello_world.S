/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal freestanding arm64/Linux payload for the offline Docker smoke. */

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94

	.section .text
	.global _start
	.type _start, %function
_start:
	mov	x0, #1
	adrp	x1, hello_message
	add	x1, x1, :lo12:hello_message
	mov	x2, #HELLO_MESSAGE_SIZE
	mov	x8, #NR_WRITE
	svc	#0

	mov	x0, #0
	mov	x8, #NR_EXIT_GROUP
	svc	#0
	brk	#0
	.size _start, .-_start

	.section .rodata
hello_message:
	.ascii "Hello from Docker!\n"
	.equ HELLO_MESSAGE_SIZE, .-hello_message
