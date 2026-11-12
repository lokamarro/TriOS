/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Implicit-register string operations and REP/REPE/REPNE loops. */

.macro string_source_address
	ldr x25, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 6 * 8)]
	tbnz x23, #3, Lx86_string_source_16\@
	tbz x23, #0, Lx86_string_source_64\@
	mov w25, w25
	b Lx86_string_source_64\@
Lx86_string_source_16\@:
	uxth w25, w25
Lx86_string_source_64\@:
	ubfx w8, w23, #1, #2
	cmp w8, #1
	b.ne Lx86_string_source_not_fs\@
	ldr x9, [_vcpu, #UML_X86_64_CPU_FS_BASE]
	b Lx86_string_source_base\@
Lx86_string_source_not_fs\@:
	cmp w8, #2
	b.ne Lx86_string_source_done\@
	ldr x9, [_vcpu, #UML_X86_64_CPU_GS_BASE]
Lx86_string_source_base\@:
	add x25, x25, x9
	/* Legacy protected mode's linear address space is 32 bits wide, so a
	 * segment base plus the offset wraps at 4 GiB there. */
	ldrb w9, [_vcpu, #UML_X86_64_CPU_EXECUTION_MODE]
	cmp w9, #1
	b.ne Lx86_string_source_done\@
	mov w25, w25
Lx86_string_source_done\@:
.endm

.macro string_destination_address
	ldr x26, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 7 * 8)]
	tbnz x23, #3, Lx86_string_destination_16\@
	tbz x23, #0, Lx86_string_destination_64\@
	mov w26, w26
	b Lx86_string_destination_64\@
Lx86_string_destination_16\@:
	uxth w26, w26
Lx86_string_destination_64\@:
.endm

.macro string_load width, load, host_register, address, destination
	mov x7, x\address
	page_cross_check (\width / 8), Lx86_string_load_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()\destination, [x7]
	b Lx86_string_load_done\@
Lx86_string_load_cross\@:
	mov x0, _context
	mov x1, x\address
	mov w2, #(\width / 8)
	helper_call uml_x86_64_gadget_cross_page_load
	cbz x1, Lx86_memory_fault
	mov x\destination, x0
Lx86_string_load_done\@:
.endm

.macro string_store width, store, host_register
	mov x7, x26
	page_cross_check (\width / 8), Lx86_string_store_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()27, [x7]
	b Lx86_string_store_done\@
Lx86_string_store_cross\@:
	mov x0, _context
	mov x1, x26
	mov x2, x27
	mov w3, #(\width / 8)
	helper_call uml_x86_64_gadget_cross_page_store
	cbz w0, Lx86_memory_fault
Lx86_string_store_done\@:
	ldrb w8, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	cbz w8, Lx86_string_store_check_overlap\@
	publish_pending_code x26, (\width / 8)
	orr x23, x23, #0x100000000
	b Lx86_string_store_ready\@
Lx86_string_store_check_overlap\@:
	ldr x8, [_context, #UML_X86_64_CONTEXT_PROGRAM_END]
	cmp x26, x8
	b.hs Lx86_string_store_ready\@
	add x9, x26, #(\width / 8)
	ldr x10, [_context, #UML_X86_64_CONTEXT_PROGRAM_BEGIN]
	cmp x9, x10
	b.ls Lx86_string_store_ready\@
	mov x0, _context
	mov x1, x26
	helper_call uml_x86_64_gadget_invalidate_code
	orr x23, x23, #0x100000000
Lx86_string_store_ready\@:
.endm

.macro string_update_pointer width, offset
	ldr x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + \offset)]
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	tbnz x9, #10, Lx86_string_pointer_decrement\@
	tbnz x23, #3, Lx86_string_pointer_increment_16\@
	tbz x23, #0, Lx86_string_pointer_increment_64\@
	add w8, w8, #(\width / 8)
	mov w8, w8
	b Lx86_string_pointer_store\@
Lx86_string_pointer_increment_16\@:
	add w10, w8, #(\width / 8)
	bfi x8, x10, #0, #16
	b Lx86_string_pointer_store\@
Lx86_string_pointer_increment_64\@:
	add x8, x8, #(\width / 8)
	b Lx86_string_pointer_store\@
Lx86_string_pointer_decrement\@:
	tbnz x23, #3, Lx86_string_pointer_decrement_16\@
	tbz x23, #0, Lx86_string_pointer_decrement_64\@
	sub w8, w8, #(\width / 8)
	mov w8, w8
	b Lx86_string_pointer_store\@
Lx86_string_pointer_decrement_16\@:
	sub w10, w8, #(\width / 8)
	bfi x8, x10, #0, #16
	b Lx86_string_pointer_store\@
Lx86_string_pointer_decrement_64\@:
	sub x8, x8, #(\width / 8)
Lx86_string_pointer_store\@:
	str x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + \offset)]
.endm



/* Move x11 bytes forward from x0 to x1, advancing both. Every block is loaded
 * whole before it is stored, so a destination below its source is exact; a
 * destination above one is the caller's to keep out of the run.
 */
.macro string_forward_copy shift
	cmp x11, #64
	b.lo Lx86_copy_tail32\@
Lx86_copy_block\@:
	ldp q0, q1, [x0], #32
	ldp q2, q3, [x0], #32
	stp q0, q1, [x1], #32
	stp q2, q3, [x1], #32
	sub x11, x11, #64
	cmp x11, #64
	b.hs Lx86_copy_block\@
Lx86_copy_tail32\@:
	/* What the block loop left is under 64 bytes, so the low bits of the
	 * count select the remaining transfers directly. An element wider than a
	 * tail cannot leave that tail behind, so those fold away here.
	 */
	tbz x11, #5, Lx86_copy_tail16\@
	ldp q0, q1, [x0], #32
	stp q0, q1, [x1], #32
Lx86_copy_tail16\@:
	tbz x11, #4, Lx86_copy_tail8\@
	ldr q0, [x0], #16
	str q0, [x1], #16
Lx86_copy_tail8\@:
	tbz x11, #3, Lx86_copy_tail4\@
	ldr x8, [x0], #8
	str x8, [x1], #8
Lx86_copy_tail4\@:
	.if \shift <= 2
	tbz x11, #2, Lx86_copy_tail2\@
	ldr w8, [x0], #4
	str w8, [x1], #4
Lx86_copy_tail2\@:
	.endif
	.if \shift <= 1
	tbz x11, #1, Lx86_copy_tail1\@
	ldrh w8, [x0], #2
	strh w8, [x1], #2
Lx86_copy_tail1\@:
	.endif
	.if \shift == 0
	tbz x11, #0, Lx86_copy_done\@
	ldrb w8, [x0]
	strb w8, [x1]
Lx86_copy_done\@:
	.endif
.endm

/* Fill x11 bytes forward at x1 with the element held in x27, advancing x1.
 * The vector form carries the element pattern and the scalar form its first
 * eight bytes; every tail is a whole number of elements, so both stay aligned
 * to the pattern.
 */
.macro string_forward_fill width, shift
	.if \width == 8
	dup v0.16b, w27
	.elseif \width == 16
	dup v0.8h, w27
	.elseif \width == 32
	dup v0.4s, w27
	.else
	dup v0.2d, x27
	.endif
	umov x8, v0.d[0]
	cmp x11, #64
	b.lo Lx86_fill_tail32\@
Lx86_fill_block\@:
	stp q0, q0, [x1], #32
	stp q0, q0, [x1], #32
	sub x11, x11, #64
	cmp x11, #64
	b.hs Lx86_fill_block\@
Lx86_fill_tail32\@:
	tbz x11, #5, Lx86_fill_tail16\@
	stp q0, q0, [x1], #32
Lx86_fill_tail16\@:
	tbz x11, #4, Lx86_fill_tail8\@
	str q0, [x1], #16
Lx86_fill_tail8\@:
	tbz x11, #3, Lx86_fill_tail4\@
	str x8, [x1], #8
Lx86_fill_tail4\@:
	.if \shift <= 2
	tbz x11, #2, Lx86_fill_tail2\@
	str w8, [x1], #4
Lx86_fill_tail2\@:
	.endif
	.if \shift <= 1
	tbz x11, #1, Lx86_fill_tail1\@
	strh w8, [x1], #2
Lx86_fill_tail1\@:
	.endif
	.if \shift == 0
	tbz x11, #0, Lx86_fill_done\@
	strb w8, [x1]
Lx86_fill_done\@:
	.endif
.endm

/* A compare leaves its operands in x11 and x12 and their difference in x13,
 * and in w19 whether they were equal, for the repeat condition. RFLAGS takes
 * them only when the instruction finishes (`Lx86_string_finish`): x86 keeps
 * the flags it started with while a repeated CMPS or SCAS is interrupted, is
 * single-stepped or faults on a later element, whatever the elements before
 * it compared. */
.macro string_compare_flags width, host_register
	mov x11, x27
	mov x12, x19
	sub \host_register\()13, \host_register\()11, \host_register\()12
	normalize_result \width, \host_register\()13
	cmp \host_register\()13, #0
	cset w19, eq
.endm

.macro string_gadget operation, width, load, store, host_register, store_register, shift
gadget string_\operation\()_\width
	ldr x23, [_stream], #8
	ubfx w8, w23, #8, #8
	cbz w8, Lx86_string_nonrepeating\@
	/* Long REP operations must periodically return to the executor so an
	 * MMU mapping-change interrupt can release its execution read gate, and
	 * so the dispatch can charge the work to its instruction budget. */
	mov w9, #UML_X86_64_STRING_REPEAT_BUDGET
	str w9, [sp, #UML_X86_64_FRAME_STRING_BUDGET]
	tbnz x23, #3, Lx86_string_count_16\@
	tbz x23, #0, Lx86_string_count_64\@
	ldr w24, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	b Lx86_string_count_ready\@
Lx86_string_count_16\@:
	ldrh w24, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	b Lx86_string_count_ready\@
Lx86_string_count_64\@:
	ldr x24, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
Lx86_string_count_ready\@:
	cbz x24, Lx86_string_finish_empty\@
	.ifc \operation,move
	b Lx86_string_bulk_run\@
	.else
	.ifc \operation,store
	b Lx86_string_bulk_run\@
	.else
	b Lx86_string_iteration\@
	.endif
	.endif
Lx86_string_nonrepeating\@:
	mov x24, #1
Lx86_string_iteration\@:
	.ifc \operation,move
	string_source_address
	string_load \width, \load, \host_register, 25, 27
	string_destination_address
	string_store \width, \store, \host_register
	string_update_pointer \width, (6 * 8)
	string_update_pointer \width, (7 * 8)
	.endif
	.ifc \operation,store
	\load \host_register\()27, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	string_destination_address
	string_store \width, \store, \host_register
	string_update_pointer \width, (7 * 8)
	.endif
	.ifc \operation,load
	string_source_address
	string_load \width, \load, \host_register, 25, 27
	\store \store_register\()27, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	string_update_pointer \width, (6 * 8)
	.endif
	.ifc \operation,compare
	string_source_address
	string_load \width, \load, \host_register, 25, 27
	string_destination_address
	string_load \width, \load, \host_register, 26, 19
	string_compare_flags \width, \host_register
	string_update_pointer \width, (6 * 8)
	string_update_pointer \width, (7 * 8)
	.endif
	.ifc \operation,scan
	\load \host_register\()27, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	string_destination_address
	string_load \width, \load, \host_register, 26, 19
	string_compare_flags \width, \host_register
	string_update_pointer \width, (7 * 8)
	.endif
	ubfx w8, w23, #8, #8
	cbz w8, Lx86_string_finish\@
	sub x24, x24, #1
	tbnz x23, #3, Lx86_string_store_count_16\@
	tbz x23, #0, Lx86_string_store_count_64\@
	mov w24, w24
Lx86_string_store_count_64\@:
	str x24, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	b Lx86_string_count_stored\@
Lx86_string_store_count_16\@:
	ldr x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	bfi x8, x24, #0, #16
	str x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
Lx86_string_count_stored\@:
	cbz x24, Lx86_string_finish\@
	.ifc \operation,compare
	cmp w8, #0xf3
	b.eq Lx86_string_repeat_equal\@
	cbnz w19, Lx86_string_finish\@
	b Lx86_string_repeat_continue\@
Lx86_string_repeat_equal\@:
	cbz w19, Lx86_string_finish\@
	.endif
	.ifc \operation,scan
	cmp w8, #0xf3
	b.eq Lx86_string_repeat_equal\@
	cbnz w19, Lx86_string_finish\@
	b Lx86_string_repeat_continue\@
Lx86_string_repeat_equal\@:
	cbz w19, Lx86_string_finish\@
	.endif
	Lx86_string_repeat_continue\@:
	ldr w9, [sp, #UML_X86_64_FRAME_STRING_BUDGET]
	subs w9, w9, #1
	str w9, [sp, #UML_X86_64_FRAME_STRING_BUDGET]
	b.eq Lx86_string_budget_spent\@
	/* Whatever sent this element to the element loop -- a short tail, an
	 * element straddling a page, a destination that can publish code -- need
	 * not hold for the elements after it, so offer the run loop every one.
	 */
	.ifc \operation,move
	b Lx86_string_bulk_run\@
	.else
	.ifc \operation,store
	b Lx86_string_bulk_run\@
	.else
	b Lx86_string_iteration\@
	.endif
	.endif
/* The instruction stops between elements; see the run loop's exit. This
 * block keeps the size it had, so the run loop's code does not move. */
Lx86_string_budget_spent\@:
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	orr x9, x9, #0x10000
	b Lx86_string_between_elements\@

/* One page-bounded forward run of a repeated MOVS or STOS.
 *
 * The element loop resolves an address, probes the software TLB and settles a
 * store's publication once per element. Those costs belong to the page rather
 * than to the element, so a run confined to one source page and one
 * destination page pays them once and then moves its bytes with ordinary host
 * accesses. Guest pages are 4 KiB here, which bounds a run at 4096 bytes and
 * keeps a copy interruptible between runs.
 *
 * Anything the run cannot express exactly -- a descending direction, address
 * arithmetic that wraps, an element split across a page, a destination that
 * publishes code, a lead too short to copy in blocks -- falls to the element
 * loop, which remains the definition of the instruction.
 */
Lx86_string_bulk_run\@:
	/* Below this a run cannot repay the translations it needs. */
	cmp x24, #(32 >> \shift)
	b.lo Lx86_string_iteration\@
	/* A descending direction is not a forward block, and 16-bit address
	 * arithmetic wraps inside a run. 32-bit arithmetic wraps at 4 GiB, which
	 * is itself a page boundary, so a run's page limit already stops short of
	 * the wrap -- but only while the linear address is the register, which a
	 * segment base need not leave true. All three are fixed for the whole
	 * instruction; the element loop rejoins here, so read them per run.
	 */
	tbnz x23, #3, Lx86_string_iteration\@
	tbz x23, #0, Lx86_string_bulk_forward\@
	tst x23, #0x6
	b.ne Lx86_string_iteration\@
Lx86_string_bulk_forward\@:
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	tbnz x9, #10, Lx86_string_iteration\@
	.ifc \operation,move
	string_source_address
	.endif
	string_destination_address
	/* Elements still to move, saturated at a page before the shift so a large
	 * count cannot overflow it, then trimmed to whichever page ends first.
	 */
	mov x19, #(4096 >> \shift)
	cmp x24, x19
	csel x19, x24, x19, lo
	.if \shift != 0
	lsl x19, x19, #\shift
	.endif
	mov x12, #4096
	.ifc \operation,move
	and x9, x25, #4095
	sub x9, x12, x9
	cmp x19, x9
	csel x19, x19, x9, ls
	.endif
	and x9, x26, #4095
	sub x9, x12, x9
	cmp x19, x9
	csel x19, x19, x9, ls
	.if \shift != 0
	/* A page boundary can fall inside an element and leave a run with no
	 * whole element in it. The element loop's page-crossing helpers move that
	 * one, and the element after it opens a page.
	 */
	and x19, x19, #~((1 << \shift) - 1)
	cbz x19, Lx86_string_iteration\@
	.endif
	.ifc \operation,move
	mov x7, x25
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	mov x27, x7
	.endif
	mov x7, x26
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
Lx86_string_bulk_ready\@:
	.ifc \operation,move
	/* Ascending element order replicates when the destination leads its
	 * source, so hold the run inside that lead. A destination below its
	 * source needs no limit: a block is loaded whole before it is stored, so
	 * a run never reads a byte it has already written.
	 */
	sub x12, x7, x27
	cbz x12, Lx86_string_bulk_copy\@
	cmp x19, x12
	csel x19, x19, x12, ls
	.if \shift != 0
	/* That lead is a distance in bytes and need not land on an element
	 * boundary, so square the run up again before it is measured. */
	and x19, x19, #~((1 << \shift) - 1)
	.endif
	cmp x19, #16
	b.lo Lx86_string_iteration\@
Lx86_string_bulk_copy\@:
	mov x0, x27
	mov x1, x7
	mov x11, x19
	string_forward_copy \shift
	.endif
	.ifc \operation,store
	\load \host_register\()27, [_vcpu, #UML_X86_64_CPU_REGISTERS]
	mov x1, x7
	mov x11, x19
	string_forward_fill \width, \shift
	.endif
	/* A destination that can hold code publishes what the run wrote, just as
	 * the element loop publishes what each element wrote, and the
	 * instruction then leaves through its own exit instead of chaining to
	 * the gadget after it. One call covers a run where the element loop
	 * needed one for every element, which is the whole reason a page of
	 * executable destination is worth running rather than stepping.
	 */
	ldrb w8, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	cbz w8, Lx86_string_bulk_overlap\@
	tbnz x23, #60, Lx86_string_bulk_exits\@
	mov x0, _context
	mov x1, x26
	mov w2, w19
	helper_call uml_x86_64_gadget_publish_code_write
Lx86_string_bulk_exits\@:
	orr x23, x23, #0x100000000
	b Lx86_string_bulk_retire\@
Lx86_string_bulk_overlap\@:
	ldr x8, [_context, #UML_X86_64_CONTEXT_PROGRAM_END]
	cmp x26, x8
	b.hs Lx86_string_bulk_retire\@
	add x9, x26, x19
	ldr x10, [_context, #UML_X86_64_CONTEXT_PROGRAM_BEGIN]
	cmp x9, x10
	b.ls Lx86_string_bulk_retire\@
	mov x0, _context
	mov x1, x26
	helper_call uml_x86_64_gadget_invalidate_code
	orr x23, x23, #0x100000000
Lx86_string_bulk_retire\@:
	/* Retire the run into architectural state before starting another, so a
	 * fault in the next translation resumes the instruction exactly here.
	 */
	.ifc \operation,move
	ldr x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 6 * 8)]
	add x8, x8, x19
	tbz x23, #0, Lx86_string_bulk_source_wide\@
	mov w8, w8
Lx86_string_bulk_source_wide\@:
	str x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 6 * 8)]
	.endif
	ldr x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 7 * 8)]
	add x8, x8, x19
	tbz x23, #0, Lx86_string_bulk_destination_wide\@
	mov w8, w8
Lx86_string_bulk_destination_wide\@:
	str x8, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 7 * 8)]
	/* The count needs no narrowing: it was loaded at its address size and a
	 * run never consumes more elements than it holds.
	 */
	.if \shift == 0
	sub x24, x24, x19
	.else
	lsr x9, x19, #\shift
	sub x24, x24, x9
	.endif
	str x24, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	cbz x24, Lx86_string_finish\@
	/* Charge the shared repeat budget by bytes moved rather than by run, so a
	 * long copy still returns to the executor often enough to release its
	 * execution read gate for a mapping change.
	 */
	ldr w9, [sp, #UML_X86_64_FRAME_STRING_BUDGET]
	lsr w10, w19, #6
	add w10, w10, #1
	subs w9, w9, w10
	str w9, [sp, #UML_X86_64_FRAME_STRING_BUDGET]
	b.gt Lx86_string_bulk_run\@
	/* The instruction stops between elements with RIP still naming it, which
	 * is where an interrupt finds it. Hardware then saves RF, so that the
	 * resumed instruction does not break again, and the runtime leaves it set
	 * if it returns to the kernel here; `execute` clears it when the
	 * instruction resumes. The instruction has retired nothing, so the exit
	 * tells the dispatch what its repeat budget covered. */
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	orr x9, x9, #0x10000
Lx86_string_between_elements\@:
	str x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	b Lx86_repeat_yield
Lx86_string_finish\@:
	.ifc \operation,compare
	record_deferred 2, \width
	.endif
	.ifc \operation,scan
	record_deferred 2, \width
	.endif
Lx86_string_finish_empty\@:
	tbz x23, #32, Lx86_string_continue\@
	b Lx86_store_code_exit
Lx86_string_continue\@:
	retire_and_next

/* A single-stepped repeated instruction, which raises #DB after each
 * iteration. One element runs, and a repeat budget of one stops the element
 * loop after it with RF set and RIP still naming the instruction, unless the
 * count or the comparison ends the instruction first, which retires it. The
 * bulk run is never entered. The gadget above keeps its layout; this entry
 * only joins it. */
gadget string_\operation\()_\width\()_step
	ldr x23, [_stream], #8
	mov w9, #1
	str w9, [sp, #UML_X86_64_FRAME_STRING_BUDGET]
	tbnz x23, #3, Lx86_string_step_count_16\@
	tbz x23, #0, Lx86_string_step_count_64\@
	ldr w24, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	b Lx86_string_step_count_ready\@
Lx86_string_step_count_16\@:
	ldrh w24, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	b Lx86_string_step_count_ready\@
Lx86_string_step_count_64\@:
	ldr x24, [_vcpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
Lx86_string_step_count_ready\@:
	cbz x24, Lx86_string_finish_empty\@
	b Lx86_string_iteration\@
.endm

.macro string_width operation, width, load, store, host_register, store_register, shift
string_gadget \operation, \width, \load, \store, \host_register, \store_register, \shift
.endm

.macro string_family operation
string_width \operation, 8, ldrb, strb, w, w, 0
string_width \operation, 16, ldrh, strh, w, w, 1
string_width \operation, 32, ldr, str, w, x, 2
string_width \operation, 64, ldr, str, x, x, 3
.endm

string_family move
string_family compare
string_family store
string_family load
string_family scan

.purgem string_family
.purgem string_width
.purgem string_gadget
.purgem string_compare_flags
.purgem string_update_pointer
.purgem string_store
.purgem string_load
.purgem string_destination_address
.purgem string_source_address
