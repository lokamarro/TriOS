// SPDX-License-Identifier: GPL-2.0-or-later
//
// SME compiler idioms for tools/arm64-tests/sme-idiom-corpus.py.
//
// Each function exercises one way compilers emit code outside streaming
// mode around SME: the prologue and epilogue of a locally streaming
// function, the mode switches around calls, streaming-compatible bodies,
// the ZA lazy-save scheme and callee-saved Z and P registers. The prefix
// names the mode at entry, which the corpus tool declares to the SM = 0
// walk: ns_ and ls_ enter with SM = 0, sc_ with either, st_ with SM = 1.
// The file is only compiled to objects, never linked: the external
// callees stay undefined. PRESSURE clobbers the callee-saved registers.

// GCC 14.2 with -fstack-clash-protection fails on the functions that switch
// mode and have an SVE-typed signature (an internal compiler error in
// aarch64_expand_prologue), so the tool retries a rejected build without
// them (SME_IDIOMS_RETRY).

#include <arm_sme.h>
#include <stddef.h>
#include <stdint.h>

#define EXTERN extern "C"

EXTERN void ext_ns(void);
EXTERN void ext_ns_args(uint8_t *pointer, int count);
EXTERN void ext_st(uint8_t *pointer, int count) __arm_streaming;
EXTERN void ext_sc(void) __arm_streaming_compatible;
EXTERN void ext_private_za(void);
EXTERN void ext_st_za(void) __arm_streaming __arm_inout("za");

#ifdef PRESSURE
#define CLOBBER_D() \
	asm volatile("" ::: "v8", "v9", "v10", "v11", "v12", "v13", "v14", \
		     "v15")
#define CLOBBER_ZP() \
	asm volatile("" ::: "z8", "z9", "z10", "z11", "z12", "z13", "z14", \
		     "z15", "z16", "z17", "z18", "z19", "z20", "z21", "z22", \
		     "z23", "p4", "p5", "p6", "p7", "p8", "p9", "p10", "p11", \
		     "p12", "p13", "p14", "p15")
#else
#define CLOBBER_D() do {} while (0)
#define CLOBBER_ZP() do {} while (0)
#endif

// A body runs in streaming mode, where Z and P exist on every SME CPU; a
// streaming-compatible or non-streaming body has them only with SVE.
#ifdef __ARM_FEATURE_SVE
#define CLOBBER_ANY() CLOBBER_ZP()
#else
#define CLOBBER_ANY() CLOBBER_D()
#endif

// Locally streaming functions: the prologue and epilogue run at SM = 0.

EXTERN __arm_locally_streaming void ls_copy_row(const uint8_t *source,
						uint8_t *destination,
						int width)
{
	CLOBBER_ZP();
	int step = (int)svcntb();
	int x = 0;
	for (; x + step <= width; x += step)
		svst1_u8(svptrue_b8(), destination + x,
			 svld1_u8(svptrue_b8(), source + x));
	svbool_t tail = svwhilelt_b8_s32(x, width);
	svst1_u8(tail, destination + x, svld1_u8(tail, source + x));
}

EXTERN __arm_locally_streaming float ls_sum(const float *values, int count)
{
	CLOBBER_ZP();
	svfloat32_t sum = svdup_n_f32(0.0f);
	for (int x = 0; x < count; x += (int)svcntw()) {
		svbool_t active = svwhilelt_b32_s32(x, count);
		sum = svadd_f32_m(active, sum, svld1_f32(active, values + x));
	}
	return svaddv_f32(svptrue_b32(), sum);
}

EXTERN __arm_locally_streaming void ls_calls_ns(uint8_t *pointer, int count)
{
	CLOBBER_ZP();
	svuint8_t kept = svld1_u8(svwhilelt_b8_s32(0, count), pointer);
	ext_ns();
	svst1_u8(svwhilelt_b8_s32(0, count), pointer, kept);
}

// Many vectors live across a call force SVE spill slots in the frame.
EXTERN __arm_locally_streaming void ls_spills(int32_t *pointer)
{
	svbool_t all = svptrue_b32();
	svint32_t v0 = svld1_vnum_s32(all, pointer, 0);
	svint32_t v1 = svld1_vnum_s32(all, pointer, 1);
	svint32_t v2 = svld1_vnum_s32(all, pointer, 2);
	svint32_t v3 = svld1_vnum_s32(all, pointer, 3);
	svint32_t v4 = svld1_vnum_s32(all, pointer, 4);
	svint32_t v5 = svld1_vnum_s32(all, pointer, 5);
	svint32_t v6 = svld1_vnum_s32(all, pointer, 6);
	svint32_t v7 = svld1_vnum_s32(all, pointer, 7);
	ext_sc();
	svint32_t sum = svadd_s32_x(all, svadd_s32_x(all, v0, v1),
				    svadd_s32_x(all, v2, v3));
	sum = svadd_s32_x(all, sum, svadd_s32_x(all, svadd_s32_x(all, v4, v5),
						svadd_s32_x(all, v6, v7)));
	svst1_s32(all, pointer, sum);
}

EXTERN __arm_locally_streaming void ls_vla(uint8_t *pointer)
{
	uint8_t scratch[svcntb()];
	svst1_u8(svptrue_b8(), scratch, svld1_u8(svptrue_b8(), pointer));
	ext_sc();
	svst1_u8(svptrue_b8(), pointer, svld1_u8(svptrue_b8(), scratch));
}

EXTERN __arm_new("za") __arm_locally_streaming void ls_new_za_mopa(
	float *result, const float *left, const float *right, int depth)
{
	CLOBBER_ZP();
	svbool_t all = svptrue_b32();
	svzero_za();
	for (int k = 0; k < depth; ++k)
		svmopa_za32_f32_m(0, all, all,
				  svld1_f32(all, left + k * svcntw()),
				  svld1_f32(all, right + k * svcntw()));
	for (uint32_t row = 0; row < svcntw(); ++row)
		svst1_hor_za32(0, row, all, result + row * svcntw());
}

// Non-streaming functions that switch mode or use ZA at SM = 0.

EXTERN void ns_calls_st(uint8_t *pointer, int count)
{
	CLOBBER_ANY();
	ext_st(pointer, count);
}

EXTERN double ns_calls_st_live(double value, uint8_t *pointer, int count)
{
	double kept = value * 3.0;
	ext_st(pointer, count);
	return kept + value;
}

EXTERN __arm_new("za") void ns_new_za_calls_st(void)
{
	CLOBBER_ANY();
	ext_st_za();
}

EXTERN void ns_inout_za_calls_private(void) __arm_inout("za")
{
	CLOBBER_ANY();
	ext_private_za();
}

EXTERN void ns_preserves_za_calls_private(void) __arm_preserves("za")
{
	ext_private_za();
}

EXTERN void ns_za_zero_ldr_str(void *buffer) __arm_inout("za")
{
	svzero_za();
	svldr_za(0, buffer);
	svstr_za(1, buffer);
}

EXTERN uint64_t ns_cntsb(void)
{
	return svcntsb();
}

#if defined(__clang__) && __clang_major__ >= 20
EXTERN void ns_agnostic_calls_private(void) __arm_agnostic("sme_za_state")
{
	ext_private_za();
}
#endif

#if defined(__ARM_FEATURE_SVE) && !defined(SME_IDIOMS_RETRY)
// An SVE-typed non-streaming signature needs FEAT_SVE whatever SME does.
EXTERN svint32_t ns_sve_pcs_calls_st(svint32_t value, uint8_t *pointer,
				     int count)
{
	CLOBBER_ZP();
	ext_st(pointer, count);
	return value;
}
#endif

// Streaming-compatible functions run at SM = 0 when the caller does.

EXTERN void sc_add_loop(int32_t *__restrict sum, const int32_t *__restrict
			addend, int count) __arm_streaming_compatible
{
	for (int x = 0; x < count; ++x)
		sum[x] += addend[x];
}

typedef int32_t v4si __attribute__((vector_size(16)));

EXTERN void sc_vector_add(v4si *sum, const v4si *addend)
	__arm_streaming_compatible
{
	*sum += *addend;
}

struct block {
	uint8_t bytes[64];
};

EXTERN void sc_struct_copy(struct block *destination,
			   const struct block *source)
	__arm_streaming_compatible
{
	*destination = *source;
}

EXTERN void sc_memset(uint8_t *pointer) __arm_streaming_compatible
{
	__builtin_memset(pointer, 0, 256);
}

EXTERN double sc_scalar_fp(double left, double right)
	__arm_streaming_compatible
{
	return left * right + __builtin_sqrt(left);
}

EXTERN void sc_calls_ns(void) __arm_streaming_compatible
{
	CLOBBER_ANY();
	ext_ns();
}

EXTERN void sc_calls_st(uint8_t *pointer, int count)
	__arm_streaming_compatible
{
	CLOBBER_ANY();
	ext_st(pointer, count);
}

#ifdef __ARM_FEATURE_SVE
// SVE intrinsics and SVE-typed signatures in a streaming-compatible
// function need FEAT_SVE outside streaming mode; compilers refuse them for
// SME without SVE.
EXTERN uint64_t sc_cntb(void) __arm_streaming_compatible
{
	return svcntb();
}

EXTERN void sc_sve_intrinsics(int32_t *pointer) __arm_streaming_compatible
{
	svbool_t all = svptrue_b32();
	svst1_s32(all, pointer,
		  svadd_n_s32_x(all, svld1_s32(all, pointer), 1));
}

EXTERN svint32_t sc_sve_pcs(svint32_t left, svint32_t right)
	__arm_streaming_compatible
{
	CLOBBER_ZP();
	return svadd_s32_x(svptrue_b32(), left, right);
}

EXTERN svbool_t sc_sve_pcs_predicate(svbool_t value)
	__arm_streaming_compatible
{
	CLOBBER_ZP();
	return svnot_b_z(value, value);
}
#endif

EXTERN void sc_za_calls_private(void) __arm_streaming_compatible
	__arm_inout("za")
{
	ext_private_za();
}

// Streaming functions leave streaming mode around non-streaming calls.

EXTERN void st_calls_ns(uint8_t *pointer, int count) __arm_streaming
{
	CLOBBER_ZP();
	svuint8_t kept = svld1_u8(svptrue_b8(), pointer);
	ext_ns_args(pointer, count);
	svst1_u8(svptrue_b8(), pointer, kept);
}

#ifndef SME_IDIOMS_RETRY
EXTERN svint32_t st_sve_pcs_calls_ns(svint32_t value) __arm_streaming
{
	CLOBBER_ZP();
	ext_ns();
	return value;
}
#endif

EXTERN void st_za_calls_private(void) __arm_streaming __arm_inout("za")
{
	ext_private_za();
}
