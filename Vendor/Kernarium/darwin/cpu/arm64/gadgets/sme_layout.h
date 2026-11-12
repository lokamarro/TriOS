/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_ARM64_GADGETS_SME_LAYOUT_H
#define UML_ARM64_GADGETS_SME_LAYOUT_H

/* Constants the gadget engine's SME part (engine/sme.inc) shares with its
 * C++ side; plain preprocessor definitions, so the assembly can include them.
 *
 * CpuState::sme_host_mode is engine-private. Besides the shared bits
 * (cpu/arm64/sme_profile.hpp), the gadget engine sets this one before it
 * enters a streaming program when the guest has ZT0, so the streaming entry
 * and the gadgets that leave and re-enter streaming mode around a helper
 * know to carry ZT0 with ZA. */
#define UML_GADGET_SME_HOST_ZT0 16

/* The operand of a ZA or ZT0 memory or zeroing gadget, which its slow path
 * hands unchanged to uml_arm64_gadget_sme_memory():
 * - bits 0-15: the CpuState offset of the base register (SP at 248);
 * - bits 16-31: the CpuState offset of the slice index register W12-W15;
 * - bits 32-35: the immediate offset of an array-vector access;
 * - bit 36: the base register is SP;
 * - bits 40-47: the tile mask of ZERO { mask };
 * - bits 48-50: the operation. */
#define UML_GADGET_SME_BASE_SP_BIT 36
#define UML_GADGET_SME_MASK_SHIFT 40
#define UML_GADGET_SME_OPERATION_SHIFT 48
#define UML_GADGET_SME_ZA_LOAD 0
#define UML_GADGET_SME_ZA_STORE 1
#define UML_GADGET_SME_ZT0_LOAD 2
#define UML_GADGET_SME_ZT0_STORE 3
#define UML_GADGET_SME_ZA_ZERO 4
#define UML_GADGET_SME_ZT0_ZERO 5

/* The stream of a vector memory gadget (engine/streaming_memory.inc): five
 * words after the entry, which the slow path hands unchanged to
 * uml_arm64_gadget_sme_vector_memory().
 * Word 0, the address:
 * - bits 0-15: the CpuState offset of the base register (SP at 248, the
 *   zero slot for a literal);
 * - bits 16-31: the CpuState offset of the index register;
 * - bits 32-47: the bytes the access spans, for the same-page check;
 * - bit 48: the base is SP; bits 49-51: the index's left shift;
 * - bit 52: the index is sign-extended; bit 53: only its low 32 bits count;
 * - bit 54: the base is written back with base + immediate;
 * - bit 55: post-indexed, so the access is at the base itself;
 * - bit 56: an index register is added.
 * Word 1: the immediate byte offset, signed (a literal's address).
 * Word 2: the register file offsets of the transfer registers, 16 bits each
 * in order; for a ZA tile slice, the CpuState offset of the slice register,
 * the slice offset, the tile and 1 for a vertical slice.
 * Word 3, the shape:
 * - bits 0-15: the register file offset of the governing predicate;
 * - bits 16-19: the shape; bit 20: a store;
 * - bits 21-23: log2 of the bytes per element in memory;
 * - bits 24-26: log2 of the bytes per element in a register;
 * - bit 27: sign-extended; bits 28-30: the registers transferred.
 * Word 4: the instruction's PC. */
#define UML_GADGET_SME_VM_FOOTPRINT_SHIFT 32
#define UML_GADGET_SME_VM_BASE_SP_BIT 48
#define UML_GADGET_SME_VM_SHIFT_SHIFT 49
#define UML_GADGET_SME_VM_SIGNED_BIT 52
#define UML_GADGET_SME_VM_WORD_BIT 53
#define UML_GADGET_SME_VM_WRITEBACK_BIT 54
#define UML_GADGET_SME_VM_POST_BIT 55
#define UML_GADGET_SME_VM_INDEX_BIT 56
#define UML_GADGET_SME_VM_SHAPE_SHIFT 16
#define UML_GADGET_SME_VM_STORE_BIT 20
#define UML_GADGET_SME_VM_MSIZE_SHIFT 21
#define UML_GADGET_SME_VM_ESIZE_SHIFT 24
#define UML_GADGET_SME_VM_SIGN_BIT 27
#define UML_GADGET_SME_VM_REGISTERS_SHIFT 28
#define UML_GADGET_SME_VM_CONTIGUOUS 0
#define UML_GADGET_SME_VM_STRUCTURE 1
#define UML_GADGET_SME_VM_REPLICATE 2
#define UML_GADGET_SME_VM_REPLICATE_QUAD 3
#define UML_GADGET_SME_VM_FILL_Z 4
#define UML_GADGET_SME_VM_FILL_P 5
#define UML_GADGET_SME_VM_ZA_SLICE 6
#define UML_GADGET_SME_VM_FP 7
/* The largest span: four registers of 256 bytes. */
#define UML_GADGET_SME_VM_MAX_FOOTPRINT 1024

/* The operand of an immediate SVCR write: the SVCR bits written in bits
 * 0-7 and their new values in bits 8-15. */
#define UML_GADGET_SME_SVCR_VALUE_SHIFT 8

/* What uml_arm64_gadget_sme_write_svcr() returns: the write took effect,
 * the task needs SME storage first (the sme_access exit), or the vCPU has
 * no register file. */
#define UML_GADGET_SME_SVCR_NEEDS_STORAGE 0
#define UML_GADGET_SME_SVCR_WRITTEN 1
#define UML_GADGET_SME_SVCR_FATAL 2

#endif
