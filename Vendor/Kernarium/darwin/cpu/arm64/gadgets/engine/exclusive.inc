/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The exclusive-access mechanism shared by every gadget family that pairs an
 * exclusive load with an exclusive store: LDXR/STXR and LDXP/STXP here, and
 * any later family (AArch32 LDREX/STREX) that builds on the same macros.
 *
 * A gadget runs the host's own exclusive instructions on the translated
 * address, so the host's exclusive monitor is the guest's. Anything that
 * runs between the load and the store and clears that monitor fails the
 * store: the helper call a TLB miss makes, and the executor's C++, whose
 * locks and atomics run whenever a block leaves to it, to translate its
 * successor, to train an indirect site, or at the block-size limit. Only an
 * exception may cost the guest its monitor, and none of those is one; a loop
 * that takes the same miss on every pass would also never complete (the
 * 4 KiB TLB is direct-mapped, so a plain store in the loop to a page that
 * shares the target's set evicts the target's write entry every pass). So
 * three rules apply:
 *
 * 1. A load probes the TLB by its WRITE tag, not its read tag. On a miss
 *    the shared miss routine asks for access 3 (Tlb::translate_exclusive_load),
 *    which translates the read, faulting as any load does, and also fills
 *    the page's write entry when the page is writable now (a failed write
 *    lookup records no fault and breaks nothing). The store that follows
 *    then hits.
 * 2. The load records a software monitor in CpuState: the host address in
 *    exclusive_key, the loaded values in exclusive_value and
 *    exclusive_value2. A store whose probe misses has lost the host monitor
 *    to its own miss, so it completes in software: the shared routine fills
 *    the TLB, then, if the recorded host address is the store's, performs
 *    one host CAS (CASP for a pair) of the recorded values and reports
 *    status 0 exactly when it succeeded. Every store clears the record.
 *    The software completion is value-based, so unlike the host monitor it
 *    cannot see a write that restored the recorded value. It runs only
 *    where the host monitor may be lost to the engine's own code.
 * 3. Each entry into translated code from the executor sets bit
 *    UML_GADGET_EXCLUSIVE_KEY_REENTERED_BIT of a live exclusive_key
 *    (program_execution.cpp). A store whose host store-exclusive fails
 *    completes in software as a miss does when the bit is set, without the
 *    TLB fill, and fails when it is clear: then only an interrupt, a page
 *    fault or a preemption of the host thread, or another observer's store,
 *    cleared the monitor, as on hardware. Both completions ignore the bit
 *    when they compare addresses.
 *
 * What a gadget supplies is small: the load calls exclusive_translate with
 * its miss stub, runs the host load, and calls exclusive_record_load (or
 * _pair); the store loads its sources into x15 (x16) before translating,
 * runs the host store, branches to its lost stub when the status in x14 is
 * not zero, then clears the record and writes the status from x14. The
 * stubs are `adr x13, <resume>; b <shared routine>`. Scratch: x8-x12, x13
 * (resume address), x14 (status), x15-x17, x20-x21; x19 and the engine
 * registers survive.
 *
 * Nothing here defines a label of its own: callers name the shared routines
 * and pass the fault exit, because sme.inc assembles the callers a second
 * time as streaming twins that rename their labels. The macros are defined
 * once; a gadget file includes this one and gets the definitions.
 */

#ifndef UML_ARM64_EXCLUSIVE_LABELS
#define UML_ARM64_EXCLUSIVE_LABELS
/* The label namespace of the exclusive families. The streaming twin copy of
 * a family redefines it, as it does for the alignment stubs. */
#define EXCL(name) Lexcl_##name
#endif

.ifndef Lexcl_mechanism_defined
.set Lexcl_mechanism_defined, 1

/* Translate the guest address in x7 to a host address in x7 by the WRITE tag
 * of its entry, branching to `miss` with x7 unchanged when it does not hold
 * the page. Scratch: x8-x12; on exit x8 is the page key and x10 the entry. */
.macro exclusive_translate miss
	lsr x8, x7, #UML_GADGET_GUEST_PAGE_SHIFT
	ubfx x9, x7, #UML_GADGET_GUEST_PAGE_SHIFT, #UML_GADGET_TLB_INDEX_BITS
	add x10, _swtlb, x9, lsl #UML_GADGET_TLB_SET_SHIFT
	ldp x11, x12, [x10, #UML_GADGET_TLB_ENTRY_WRITE_PAGE]
	cmp x8, x11
	b.ne \miss
	add x7, x12, x7
.endm

/* Record the software monitor after a host load: host address x7, value x15
 * (and x16). Scratch: x9. */
.macro exclusive_record_load
	add x9, _vcpu, #UML_GADGET_CPU_EXCLUSIVE_KEY
	stp x7, x15, [x9]
.endm

.macro exclusive_record_load_pair
	add x9, _vcpu, #UML_GADGET_CPU_EXCLUSIVE_KEY
	stp x7, x15, [x9]
	str x16, [x9, #(UML_GADGET_CPU_EXCLUSIVE_VALUE2 - UML_GADGET_CPU_EXCLUSIVE_KEY)]
.endm

/* Every store ends the monitor, whatever its outcome. A zero host address
 * matches no store. */
.macro exclusive_record_clear
	str xzr, [_vcpu, #UML_GADGET_CPU_EXCLUSIVE_KEY]
.endm

/* The load's shared miss routine: x7 guest address, x13 where the gadget
 * resumes with x7 the host address. The routine leaves the block through
 * `fault` when the page cannot be read. Both helper-call forms keep CpuState
 * at the top of the stack, as the streaming twin's trampoline expects. */
.macro exclusive_load_miss entry, fault
\entry:
	stp x1, x2, [sp, #-32]!
	str x13, [sp, #16]
	mov x0, x2
	mov x1, x7
	mov w2, #3
	helper_call uml_arm64_tlb_miss
	ldr x13, [sp, #16]
	ldp x1, x2, [sp], #32
	cbnz x0, 1f
	b \fault
1:
	mov x7, x0
	br x13
.endm

/* The entry of a store's shared completion routine for a host store-exclusive
 * that failed: x7 the host address, x15 (x16) the value, w14 the failure.
 * Unless the record is marked, the failure stands; otherwise it continues
 * at the routine's comparison, label 3, with the record in x9. */
.macro exclusive_store_lost entry
\entry:
	ldr x9, [_vcpu, #UML_GADGET_CPU_EXCLUSIVE_KEY]
	tbnz x9, #UML_GADGET_EXCLUSIVE_KEY_REENTERED_BIT, 3f
	br x13
.endm

/* The store's shared completion routine for one width and ordering: x7 guest
 * address, x15 the value to store, x13 where the gadget continues with the
 * status in x14. `cas_instruction` is the CAS of the width and ordering.
 * `lost` is its entry for a failed host store-exclusive. */
.macro exclusive_store_complete entry, lost, cas_instruction, host_register, fault
	exclusive_store_lost \lost
\entry:
	stp x1, x2, [sp, #-32]!
	stp x15, x13, [sp, #16]
	mov x0, x2
	mov x1, x7
	mov w2, #1
	helper_call uml_arm64_tlb_miss
	ldp x15, x13, [sp, #16]
	ldp x1, x2, [sp], #32
	cbnz x0, 1f
	b \fault
1:
	mov x7, x0
	ldr x9, [_vcpu, #UML_GADGET_CPU_EXCLUSIVE_KEY]
3:
	and x9, x9, #~(1 << UML_GADGET_EXCLUSIVE_KEY_REENTERED_BIT)
	cmp x9, x7
	b.ne 2f
	ldr x10, [_vcpu, #UML_GADGET_CPU_EXCLUSIVE_VALUE]
	mov x11, x10
	\cas_instruction \host_register\()11, \host_register\()15, [x7]
	cmp \host_register\()11, \host_register\()10
	cset w14, ne
	br x13
2:
	mov w14, #1
	br x13
.endm

/* The pair store's completion: x15 and x16 hold the pair. CASP wants even
 * and odd consecutive registers, so the new pair moves to x16:x17 and the
 * recorded one to x10:x11, with copies in x20:x21 to compare against. */
.macro exclusive_pair_store_complete entry, lost, cas_instruction, host_register, fault
	exclusive_store_lost \lost
\entry:
	stp x1, x2, [sp, #-48]!
	stp x15, x16, [sp, #16]
	str x13, [sp, #32]
	mov x0, x2
	mov x1, x7
	mov w2, #1
	helper_call uml_arm64_tlb_miss
	ldr x13, [sp, #32]
	ldp x15, x16, [sp, #16]
	ldp x1, x2, [sp], #48
	cbnz x0, 1f
	b \fault
1:
	mov x7, x0
	ldr x9, [_vcpu, #UML_GADGET_CPU_EXCLUSIVE_KEY]
3:
	and x9, x9, #~(1 << UML_GADGET_EXCLUSIVE_KEY_REENTERED_BIT)
	cmp x9, x7
	b.ne 2f
	add x12, _vcpu, #UML_GADGET_CPU_EXCLUSIVE_VALUE
	ldp x10, x11, [x12]
	mov x20, x10
	mov x21, x11
	mov x17, x16
	mov x16, x15
	\cas_instruction \host_register\()10, \host_register\()11, \host_register\()16, \host_register\()17, [x7]
	cmp \host_register\()10, \host_register\()20
	ccmp \host_register\()11, \host_register\()21, #0, eq
	cset w14, ne
	br x13
2:
	mov w14, #1
	br x13
.endm

.endif
