/* SPDX-License-Identifier: GPL-2.0-or-later */
	.intel_syntax noprefix
	.global _start
	.section .text

_start:
	/* Validate the x86-64 arch_prctl path and FS-relative addressing. */
	mov eax, 158
	mov edi, 0x1002
	lea rsi, [rip + tls_word]
	syscall
	test eax, eax
	jne failure
	mov rax, QWORD PTR fs:0
	mov rbx, 0x1122334455667788
	cmp rax, rbx
	jne failure

	/* Seed every floating-point class carried by the Linux x86 signal ABI. */
	vmovdqu ymm0, YMMWORD PTR [rip + vector_value]
	fld QWORD PTR [rip + x87_value]
	ldmxcsr DWORD PTR [rip + mxcsr_value]

	/* Install a handler without SA_RESTORER so Darwin's synthetic return
	 * boundary and the x86-64 rt_sigframe are both exercised.
	 */
	mov eax, 13
	mov edi, 10
	lea rsi, [rip + signal_action]
	xor edx, edx
	mov r10d, 8
	syscall
	test eax, eax
	jne failure

	mov eax, 39
	syscall
	mov edi, eax
	mov esi, 10
	mov eax, 62
	syscall
	test eax, eax
	jne failure
	mov rax, QWORD PTR [rip + signal_seen]
	cmp rax, 1
	jne failure

	/* The handler deliberately destroyed its live FP state. rt_sigreturn must
	 * restore all 256 vector bits, x87, and MXCSR from the XSAVE frame.
	 */
	vmovdqu YMMWORD PTR [rip + vector_result], ymm0
	mov rax, QWORD PTR [rip + vector_result]
	cmp rax, QWORD PTR [rip + vector_value]
	jne failure
	mov rax, QWORD PTR [rip + vector_result + 8]
	cmp rax, QWORD PTR [rip + vector_value + 8]
	jne failure
	mov rax, QWORD PTR [rip + vector_result + 16]
	cmp rax, QWORD PTR [rip + vector_value + 16]
	jne failure
	mov rax, QWORD PTR [rip + vector_result + 24]
	cmp rax, QWORD PTR [rip + vector_value + 24]
	jne failure
	fstp QWORD PTR [rip + x87_result]
	mov rax, QWORD PTR [rip + x87_result]
	cmp rax, QWORD PTR [rip + x87_value]
	jne failure
	stmxcsr DWORD PTR [rip + mxcsr_result]
	mov eax, DWORD PTR [rip + mxcsr_result]
	cmp eax, DWORD PTR [rip + mxcsr_value]
	jne failure

	mov eax, 1
	mov edi, 1
	lea rsi, [rip + success_message]
	mov edx, OFFSET success_message_size
	syscall

	/* LINUX_REBOOT_CMD_POWER_OFF. */
	mov eax, 169
	mov edi, 0xfee1dead
	mov esi, 0x28121969
	mov edx, 0x4321fedc
	xor r10d, r10d
	syscall

	/* A successful reboot does not return. */
	xor edi, edi
	mov eax, 231
	syscall

failure:
	mov eax, 1
	mov edi, 2
	lea rsi, [rip + failure_message]
	mov edx, OFFSET failure_message_size
	syscall
	mov edi, 1
	mov eax, 231
	syscall

signal_handler:
	/* ucontext::uc_mcontext.fpstate points at a 64-byte-aligned Linux x86
	 * XSAVE frame with x87, SSE, and AVX enabled.
	 */
	test DWORD PTR [rdx], 1
	jz signal_failure
	mov rax, QWORD PTR [rdx + 224]
	test rax, rax
	jz signal_failure
	test rax, 63
	jnz signal_failure
	cmp DWORD PTR [rax + 464], 0x46505853
	jne signal_failure
	cmp DWORD PTR [rax + 468], 836
	jne signal_failure
	cmp QWORD PTR [rax + 472], 7
	jne signal_failure
	cmp DWORD PTR [rax + 480], 832
	jne signal_failure
	cmp QWORD PTR [rax + 512], 7
	jne signal_failure
	cmp DWORD PTR [rax + 832], 0x46505845
	jne signal_failure

	/* A signal handler starts from architectural FP init state. Destroy that
	 * state so the return path must consume the saved frame rather than leak
	 * the handler's registers back to the interrupted context.
	 */
	vpxor ymm0, ymm0, ymm0
	fninit
	ldmxcsr DWORD PTR [rip + mxcsr_default]
	mov eax, 1
	mov QWORD PTR [rip + signal_seen], rax
	ret

signal_failure:
	mov eax, 2
	mov QWORD PTR [rip + signal_seen], rax
	ret

	.section .rodata
success_message:
	.ascii "x86_64 gadgets rootfs: syscall tls signal xstate ok\n"
success_message_end:
	.equ success_message_size, success_message_end - success_message
failure_message:
	.ascii "x86_64 gadgets rootfs: validation failed\n"
failure_message_end:
	.equ failure_message_size, failure_message_end - failure_message

	.section .data
	.align 8
tls_word:
	.quad 0x1122334455667788
signal_seen:
	.quad 0
signal_action:
	.quad signal_handler
	.quad 0
	.quad 0
	.quad 0
	.align 32
vector_value:
	.quad 0x0123456789abcdef
	.quad 0xfedcba9876543210
	.quad 0x0f1e2d3c4b5a6978
	.quad 0x8877665544332211
x87_value:
	.quad 0x3ff8000000000000
mxcsr_value:
	.long 0x00005f80
mxcsr_default:
	.long 0x00001f80
	.align 32
vector_result:
	.zero 32
x87_result:
	.zero 8
mxcsr_result:
	.zero 4
