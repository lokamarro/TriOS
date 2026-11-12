/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The x86-64 gadget region: the bank and the cold part, each in a section of
 * its own. Every image that carries the engine links UML::GadgetRegionLayout,
 * which merges them into __TEXT,__text ahead of all other code but the arm64
 * gadget region, so in an image that carries both engines the x86-64 region
 * follows the arm64 one. Each part starts on a 16 KiB host page.
 *
 * Each part is a run of slots, one per group in the table below: common's
 * entry code, the veneer island, each baked family and each engine .inc
 * file, in the table's order. A group begins at its slot's start plus its
 * phase, and every slot is a whole number of 128-byte lines, so a group keeps
 * its offset, its bytes and the distance of every branch to another group
 * while any other group changes size within its own slot. The bytes between a
 * group's last instruction and the next group's first are zero: they follow
 * an unconditional branch and are never reached. docs/architecture.md has the
 * contract and darwin/cpu/x86_64/gadgets/README.md the rule for changing it.
 *
 * The phases are the 128-byte phases the groups had in the macOS x86_64
 * product at 6152e368, where each began wherever the code before it ended; the
 * island was new. Each slot then took 1 KiB beyond its group's phase and size,
 * rounded up to a line. */

#define GADGET_SECTION_BANK __TEXT,__x86_bank,regular,pure_instructions
#define GADGET_SECTION_COLD __TEXT,__x86_cold,regular,pure_instructions

/* One row of the table: a group's phase and slot in the bank, then in the
 * cold part. A group with no cold code has no cold slot. */
	.set Lx86_slots, 0
.macro gadget_slot name, bank_phase, bank_slot, cold_phase, cold_slot
	.if (\bank_phase > 127) || (\cold_phase > 127) || (\bank_slot % 128) || (\cold_slot % 128)
	.error "x86-64 gadget slot \name: a phase is below 128 and a slot whole 128-byte lines"
	.endif
	.set Lx86_slot_\name, Lx86_slots
	.set Lx86_slots, Lx86_slots + 1
	.set Lx86_bank_phase_\name, \bank_phase
	.set Lx86_bank_slot_\name, \bank_slot
	.set Lx86_cold_phase_\name, \cold_phase
	.set Lx86_cold_slot_\name, \cold_slot
.endm

/*	     group                     bank phase  slot   cold phase  slot */
	gadget_slot common,                     64, 0x003780,   0, 0x00000
	gadget_slot veneer,                      0, 0x000100,   0, 0x00000
	gadget_slot baked_shared_tails,        124, 0x000a00,   0, 0x00000
	gadget_slot baked_arith_register,       52, 0x073300,   0, 0x00000
	gadget_slot baked_move_register,        56, 0x009f00,   0, 0x00000
	gadget_slot baked_extend_register,      52, 0x019480,   0, 0x00000
	gadget_slot baked_unary_register,       52, 0x00f100,   0, 0x00000
	gadget_slot baked_addresses,            56, 0x008c80,   0, 0x00000
	gadget_slot baked_loads,                52, 0x067780,   0, 0x00000
	gadget_slot baked_stores,               52, 0x040100,   0, 0x09200
	gadget_slot baked_memory_arith,         24, 0x3cff00,  64, 0x2c900
	gadget_slot baked_lea,                  96, 0x004b80,   0, 0x00000
	gadget_slot baked_push_pop,             96, 0x001e80,   0, 0x00600
	gadget_slot load_store,                 44, 0x006700,   0, 0x01d80
	gadget_slot core_integer,               68, 0x01c580, 104, 0x08100
	gadget_slot strings,                     4, 0x005c00,  72, 0x00780
	gadget_slot shifts,                     64, 0x00bf80,  88, 0x02e00
	gadget_slot multiplication,             88, 0x001800,  56, 0x00780
	gadget_slot division,                    0, 0x000d80,   0, 0x00600
	gadget_slot atomics,                    12, 0x002a00,  96, 0x00900
	gadget_slot bits,                       36, 0x008c00, 104, 0x02700
	gadget_slot scalar_extensions,          16, 0x001580,  44, 0x00800
	gadget_slot bmi,                        56, 0x002680,  72, 0x01180
	gadget_slot floating_simd,             124, 0x001c00,  96, 0x00d80
	gadget_slot vector_moves,               28, 0x001100,  20, 0x00700
	gadget_slot vector_lanes,               80, 0x000d00,  36, 0x00600
	gadget_slot vector_shuffle,             68, 0x003f80,  72, 0x02680
	gadget_slot vector_float,              116, 0x022500,   8, 0x0ac80
	gadget_slot vector_convert,             76, 0x002f00, 112, 0x01000
	gadget_slot vector_scalar_convert,      68, 0x002700,   8, 0x00d80
	gadget_slot vector_fma,                 76, 0x021d00,  88, 0x2dd00
	gadget_slot vector_aes,                104, 0x001180,  72, 0x00680
	gadget_slot vector_sha,                120, 0x000f00, 100, 0x00600
	gadget_slot vector_integer,            100, 0x011700,  76, 0x05b80
	gadget_slot vector_integer_map2,        92, 0x00c180,  20, 0x04680
	gadget_slot vector_map2_masked_memory, 120, 0x000e80,   4, 0x00600
	gadget_slot vector_map2_misc,          116, 0x002f00, 100, 0x01300
	gadget_slot vector_gather,              16, 0x000f00,  48, 0x00680
	gadget_slot vector_map3_lanes,           8, 0x001480,  16, 0x00700
	gadget_slot vector_map3_shuffle,         4, 0x006200,  56, 0x01c80
	gadget_slot vector_map3_round,         112, 0x002680, 120, 0x00700
	gadget_slot vector_map3_math,           84, 0x001580,  16, 0x02000
	gadget_slot vector_map3_crypto,        100, 0x000f00,  72, 0x00580
	gadget_slot vector_map3_strings,        88, 0x001400,  56, 0x00580
	gadget_slot vector_extract,              0, 0x000b00,  40, 0x00500
	gadget_slot vector_test,                 8, 0x001000,  92, 0x00600
	gadget_slot vector_compare,              4, 0x000c00,  68, 0x00580
	gadget_slot vector_mask_store,         108, 0x000900,  52, 0x00500
	gadget_slot control,                    64, 0x008500,  44, 0x02600
	gadget_slot legacy_control,              8, 0x002980, 108, 0x00780
	gadget_slot legacy_system,              28, 0x001e00,  36, 0x00700
	gadget_slot extended_state,             40, 0x000e80,   0, 0x00500
	gadget_slot x87_common,                 56, 0x000480,   0, 0x00000
	gadget_slot x87_state,                  68, 0x001500,   0, 0x00000
	gadget_slot x87_memory,                 20, 0x003700, 112, 0x01600
	gadget_slot x87_arithmetic,             36, 0x007880, 120, 0x01480
	gadget_slot x87_transcendental,         92, 0x001c00,   0, 0x00000
	gadget_slot system,                     16, 0x000d80, 120, 0x00580
	gadget_slot vector_dot,                  0, 0x001200,   0, 0x00800

/* The parts' offset-0 labels, which the order file names. The cold part's is
 * also what a host sample of a cold path reads, its own labels being
 * temporary. CSYM() is not defined until engine/common.inc. */
.macro gadget_region_begin
	.section GADGET_SECTION_BANK
	.p2align 14
	.private_extern _uml_x86_64_gadget_region_bank
_uml_x86_64_gadget_region_bank:
	.pushsection GADGET_SECTION_COLD
	.p2align 14
	.private_extern _uml_x86_64_gadget_cold_section_begin
_uml_x86_64_gadget_cold_section_begin:
	.popsection
.endm

/* Open the table's next group in both parts and close the one before it. A
 * group that outgrows its slot fails assembly at the `.org` that closes it,
 * which names the group. */
	.set Lx86_groups, 0
	.set Lx86_bank_cursor, 0
	.set Lx86_cold_cursor, 0
.macro gadget_group_close
.endm
.macro gadget_group name
	gadget_group_close
	.purgem gadget_group_close
	.if Lx86_slot_\name != Lx86_groups
	.error "x86-64 gadget group \name is not the slot table's next row"
	.endif
	.set Lx86_groups, Lx86_groups + 1
	.org Lx86_bank_cursor + Lx86_bank_phase_\name
	.set Lx86_bank_end_\name, Lx86_bank_cursor + Lx86_bank_slot_\name
	.set Lx86_bank_cursor, Lx86_bank_end_\name
	.pushsection GADGET_SECTION_COLD
	.org Lx86_cold_cursor + Lx86_cold_phase_\name
	.popsection
	.set Lx86_cold_end_\name, Lx86_cold_cursor + Lx86_cold_slot_\name
	.set Lx86_cold_cursor, Lx86_cold_end_\name
.macro gadget_group_close
	.org Lx86_bank_end_\name
	.pushsection GADGET_SECTION_COLD
	.org Lx86_cold_end_\name
	.popsection
.endm
.endm

.macro gadget_region_end
	gadget_group_close
	.if Lx86_groups != Lx86_slots
	.error "the x86-64 gadget slot table has a row no group opens"
	.endif
.endm
