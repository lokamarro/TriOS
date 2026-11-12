/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Keep one translation unit so shared macros and local labels remain cheap. */
/* Each group opens its slot in the region (engine/region.inc), in the slot
 * table's order. */
#include "engine/region.inc"
	gadget_region_begin
	gadget_group common
#include "engine/common.inc"
/* The veneers of the helpers outside the engine (engine/common.inc). */
	gadget_group veneer
	gadget_veneers
/* Generated at build time by engine/generate_baked.py, which opens a group
 * per family. It follows the island because it is the largest and hottest
 * scalar code and changes least, so growing a later slot never moves it. */
#include "baked_scalar.inc"
	gadget_group load_store
#include "engine/load_store.inc"
	gadget_group core_integer
#include "engine/core_integer.inc"
	gadget_group strings
#include "engine/strings.inc"
	gadget_group shifts
#include "engine/shifts.inc"
	gadget_group multiplication
#include "engine/multiplication.inc"
	gadget_group division
#include "engine/division.inc"
	gadget_group atomics
#include "engine/atomics.inc"
	gadget_group bits
#include "engine/bits.inc"
	gadget_group scalar_extensions
#include "engine/scalar_extensions.inc"
	gadget_group bmi
#include "engine/bmi.inc"
	gadget_group floating_simd
#include "engine/floating_simd.inc"
	gadget_group vector_moves
#include "engine/vector_moves.inc"
	gadget_group vector_lanes
#include "engine/vector_lanes.inc"
	gadget_group vector_shuffle
#include "engine/vector_shuffle.inc"
	gadget_group vector_float
#include "engine/vector_float.inc"
	gadget_group vector_convert
#include "engine/vector_convert.inc"
	gadget_group vector_scalar_convert
#include "engine/vector_scalar_convert.inc"
	gadget_group vector_fma
#include "engine/vector_fma.inc"
	gadget_group vector_aes
#include "engine/vector_aes.inc"
	gadget_group vector_sha
#include "engine/vector_sha.inc"
	gadget_group vector_integer
#include "engine/vector_integer.inc"
	gadget_group vector_integer_map2
#include "engine/vector_integer_map2.inc"
	gadget_group vector_map2_masked_memory
#include "engine/vector_map2_masked_memory.inc"
	gadget_group vector_map2_misc
#include "engine/vector_map2_misc.inc"
	gadget_group vector_gather
#include "engine/vector_gather.inc"
	gadget_group vector_map3_lanes
#include "engine/vector_map3_lanes.inc"
	gadget_group vector_map3_shuffle
#include "engine/vector_map3_shuffle.inc"
	gadget_group vector_map3_round
#include "engine/vector_map3_round.inc"
	gadget_group vector_map3_math
#include "engine/vector_map3_math.inc"
	gadget_group vector_map3_crypto
#include "engine/vector_map3_crypto.inc"
	gadget_group vector_map3_strings
#include "engine/vector_map3_strings.inc"
	gadget_group vector_extract
#include "engine/vector_extract.inc"
	gadget_group vector_test
#include "engine/vector_test.inc"
	gadget_group vector_compare
#include "engine/vector_compare.inc"
	gadget_group vector_mask_store
#include "engine/vector_mask_store.inc"
	gadget_group control
#include "engine/control.inc"
	gadget_group legacy_control
#include "engine/legacy_control.inc"
	gadget_group legacy_system
#include "engine/legacy_system.inc"
	gadget_group extended_state
#include "engine/extended_state.inc"
	gadget_group x87_common
#include "engine/x87_common.inc"
	gadget_group x87_state
#include "engine/x87_state.inc"
	gadget_group x87_memory
#include "engine/x87_memory.inc"
	gadget_group x87_arithmetic
#include "engine/x87_arithmetic.inc"
	gadget_group x87_transcendental
#include "engine/x87_transcendental.inc"
	gadget_group system
#include "engine/system.inc"
	gadget_group vector_dot
#include "engine/vector_dot.inc"
	gadget_region_end
