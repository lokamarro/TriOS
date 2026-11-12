// SPDX-License-Identifier: GPL-2.0-or-later
/* Group "simd": VFP and Advanced SIMD state and semantics.
 * D0-D31 and FPSCR survive system calls, rescheduling, fork, threads and
 * signals (in the frame too, which the handler can change); the FPSCR control
 * bits (rounding, flush to zero, default NaN, alternative half precision)
 * round trip and act; the cumulative flags, the compare flags and QC are set
 * as the architecture says; Advanced SIMD floating point runs under the
 * Standard FPSCR value; VLDR, VLDM and aligned VLD1 raise SIGBUS when
 * misaligned; Len, Stride and the trap enables are RAZ/WI; VFP and NEON data
 * are visible through the S, D and Q views and VPUSH/VPOP; and a structure
 * load, store or permute alone in its block publishes its registers to the
 * blocks around it (across a reschedule, in A32 and T32, cold and warm). */
#include "engine_probe.hpp"

namespace probe {

/* The argument block of the arithmetic routines in engine_probe_simd.S. */
struct FpCall {
	u64 a, b, c, result;
	u32 fpscr_in, fpscr_out;
};
static_assert(sizeof(FpCall) == 40);

} // namespace probe

extern "C" {
void simd_load(const probe::u64 *d);
void simd_store(probe::u64 *d);
void simd_load_thumb(const probe::u64 *d);
void simd_store_thumb(probe::u64 *d);
probe::u32 simd_get_fpscr();
void simd_set_fpscr(probe::u32 value);
void simd_clobber(probe::u32 fpscr);
void simd_spin(probe::u32 n, probe::u32 *out);
void simd_alias(probe::u32 *out);
void simd_pushpop(probe::u32 *out);
void simd_bkpt();
void fp_add_f32(probe::FpCall *);
void fp_sub_f32(probe::FpCall *);
void fp_mul_f32(probe::FpCall *);
void fp_div_f32(probe::FpCall *);
void fp_fma_f32(probe::FpCall *);
void fp_add_f64(probe::FpCall *);
void fp_mul_f64(probe::FpCall *);
void fp_div_f64(probe::FpCall *);
void fp_fma_f64(probe::FpCall *);
void fp_sqrt_f32(probe::FpCall *);
void fp_cvtr_s32_f32(probe::FpCall *);
void fp_cvt_s32_f32(probe::FpCall *);
void fp_cvt_u32_f32(probe::FpCall *);
void fp_cvtb_f16_f32(probe::FpCall *);
void fp_cvtb_f32_f16(probe::FpCall *);
void fp_cmp_f32(probe::FpCall *);
void fp_cmpe_f32(probe::FpCall *);
void fp_cmp_f64(probe::FpCall *);
void fp_cmp_apsr_f32(probe::FpCall *);
void fp_vrintn_f32(probe::FpCall *);
void fp_vrinta_f32(probe::FpCall *);
void fp_vrintp_f32(probe::FpCall *);
void fp_vrintm_f32(probe::FpCall *);
void fp_vrintz_f32(probe::FpCall *);
void fp_vrintx_f32(probe::FpCall *);
void fp_vrintr_f32(probe::FpCall *);
void fp_vcvta_s32_f32(probe::FpCall *);
void fp_vcvtn_s32_f32(probe::FpCall *);
void fp_vcvtp_s32_f32(probe::FpCall *);
void fp_vcvtm_s32_f32(probe::FpCall *);
void fp_vmaxnm_f32(probe::FpCall *);
void fp_vminnm_f32(probe::FpCall *);
void fp_vmla_f32(probe::FpCall *);
void fp_vcvt_f64_f32(probe::FpCall *);
void fp_vcvt_f32_s32(probe::FpCall *);
void neon_add_f32(probe::FpCall *);
void neon_mul_f32(probe::FpCall *);
void neon_qadd_s8(probe::FpCall *);
void neon_add_i32(probe::FpCall *);
#define SPLIT_FORMS(X) \
	X(vld1_8) X(vld2_16) X(vld3_16) X(vld4_32) X(vld1_lane) X(vld2_lane) X(vld4_dup) \
	X(vst1_64) X(vst2_16) X(vst3_8) X(vst4_32) X(vst1_lane) X(vzip_32) X(vtbl_8)
#define SPLIT_DECLARE(form) \
	void split_##form##_arm(void *); void split_##form##_thumb(void *); \
	void fused_##form##_arm(void *, const probe::u64 *, probe::u64 *); \
	void fused_##form##_thumb(void *, const probe::u64 *, probe::u64 *);
SPLIT_FORMS(SPLIT_DECLARE)
#undef SPLIT_DECLARE
#define SA(name) void name##_arm(probe::word); void name##_thumb(probe::word);
SA(sa_vldr_s) SA(sa_vldr_d) SA(sa_vstr_s) SA(sa_vstr_d) SA(sa_vldm_s) SA(sa_vldm_d)
SA(sa_vstm_d) SA(sa_vld1_a64) SA(sa_vld1_a128) SA(sa_vld1_a256) SA(sa_vst1_a64)
SA(sa_vld1_free) SA(sa_vld1_byte) SA(sa_vst1_free) SA(sa_vld3_free) SA(sa_vld1_lane)
#undef SA
}

namespace probe {
namespace {

constexpr u32 rn = 0, rp = 0x00400000, rm = 0x00800000, rz = 0x00c00000;
constexpr u32 fz = 1U << 24, dn = 1U << 25, ahp = 1U << 26;
constexpr u32 ioc = 1, dzc = 2, ofc = 4, ufc = 8, ixc = 0x10, idc = 0x80, qc = 1U << 27;
constexpr u32 cumulative = 0x9f;
constexpr int sigfpe = 8;

/* ---- the register image ---- */

struct Image {
	alignas(8) u64 d[32];
};

void make_pattern(Image &image, u32 seed)
{
	for (u32 index = 0; index < 32; ++index) {
		const u32 low = 0xa5000000U ^ (seed << 16) ^ (index << 8) ^ (index * 0x37 + seed);
		const u32 high = 0x5a000000U ^ (seed << 8) ^ (index << 16) ^ (seed * 0x1d + index);
		image.d[index] = (static_cast<u64>(high) << 32) | low;
	}
}

u32 mismatches(const u64 *got, const u64 *want)
{
	u32 bad = 0;
	for (u32 index = 0; index < 32; ++index)
		if (got[index] != want[index])
			++bad;
	return bad;
}

/* Reads back the register image through the VFP routine. */
u32 image_mismatches(const Image &want)
{
	Image got{};
	simd_store(got.d);
	return mismatches(got.d, want.d);
}

/* Holds the image for `rounds` system calls and reschedules, counting the
 * rounds in which it changed (re-checks every 8th). */
u32 hold_image(const Image &want, u32 fpscr, u32 rounds)
{
	u32 bad = 0;
	for (u32 round = 0; round < rounds; ++round) {
		yield();
		if (round % 16 == 0)
			sleep_ms(1);
		if (getpid() <= 0)
			++bad;
		if (round % 8 == 0 && (image_mismatches(want) != 0 || (simd_get_fpscr() & 0xffc8009f) != fpscr))
			++bad;
	}
	return bad;
}

/* ---- frames in signal handlers ---- */

struct HandlerResult {
	u32 deliveries;
	u32 magic_ok;
	u32 size_ok;
	u32 image_bad;
	u32 fpscr_frame;
	u32 fpscr_frame_bad;
	Image expect;
	u32 expect_fpscr;
	bool modify;
	u64 new_d3;
	u32 new_fpscr;
};
HandlerResult handler_result;

VfpFrame *frame_of(UContext *context)
{
	return reinterpret_cast<VfpFrame *>(context->regspace);
}

void frame_handler(int, SigInfo *, UContext *context)
{
	HandlerResult &r = handler_result;
	++r.deliveries;
	VfpFrame *frame = frame_of(context);
	if (frame->magic == vfp_magic)
		++r.magic_ok;
	if (frame->size == sizeof(VfpFrame))
		++r.size_ok;
	r.image_bad += mismatches(frame->d, r.expect.d);
	r.fpscr_frame = frame->fpscr;
	if ((frame->fpscr & 0xffc8009f) != r.expect_fpscr)
		++r.fpscr_frame_bad;
	if (r.modify) {
		frame->d[3] = r.new_d3;
		frame->fpscr = r.new_fpscr;
	}
	/* Everything the handler leaves behind has to be undone by sigreturn. */
	simd_clobber(0x03c00000);
}

u64 double_bits(u32 n)
{
	/* n in [2^20, 2^21): the bits of (double)n. */
	return (0x413ULL << 52) | (static_cast<u64>(n - (1U << 20)) << 32);
}

void alarm_handler(int, SigInfo *, UContext *context)
{
	HandlerResult &r = handler_result;
	++r.deliveries;
	VfpFrame *frame = frame_of(context);
	if (frame->magic == vfp_magic && frame->size == sizeof(VfpFrame))
		++r.magic_ok;
	simd_clobber(0x03c00000);
}

/* ---- arithmetic cases ---- */

struct Op {
	void (*function)(FpCall *);
	u64 a, b, c;
	u32 fpscr;
	u64 result;
	u64 mask;
	u32 flags;
	const char *name;
};

constexpr u64 all32 = 0xffffffffULL;
constexpr u64 all64 = ~0ULL;
constexpr u64 low16 = 0xffffULL;

const Op ops[] = {
	/* Rounding modes: 1/3 and -1/3 in single and double precision. */
	{fp_div_f32, 0x3f800000, 0x40400000, 0, rn, 0x3eaaaaab, all32, ixc, "1/3 under round to nearest"},
	{fp_div_f32, 0x3f800000, 0x40400000, 0, rp, 0x3eaaaaab, all32, ixc, "1/3 under round up"},
	{fp_div_f32, 0x3f800000, 0x40400000, 0, rm, 0x3eaaaaaa, all32, ixc, "1/3 under round down"},
	{fp_div_f32, 0x3f800000, 0x40400000, 0, rz, 0x3eaaaaaa, all32, ixc, "1/3 under round to zero"},
	{fp_div_f32, 0xbf800000, 0x40400000, 0, rn, 0xbeaaaaab, all32, ixc, "-1/3 under round to nearest"},
	{fp_div_f32, 0xbf800000, 0x40400000, 0, rp, 0xbeaaaaaa, all32, ixc, "-1/3 under round up"},
	{fp_div_f32, 0xbf800000, 0x40400000, 0, rm, 0xbeaaaaab, all32, ixc, "-1/3 under round down"},
	{fp_div_f32, 0xbf800000, 0x40400000, 0, rz, 0xbeaaaaaa, all32, ixc, "-1/3 under round to zero"},
	{fp_div_f64, 0x3ff0000000000000ULL, 0x4008000000000000ULL, 0, rn, 0x3fd5555555555555ULL, all64, ixc,
	 "double 1/3 under round to nearest"},
	{fp_div_f64, 0x3ff0000000000000ULL, 0x4008000000000000ULL, 0, rp, 0x3fd5555555555556ULL, all64, ixc,
	 "double 1/3 under round up"},
	{fp_div_f64, 0x3ff0000000000000ULL, 0x4008000000000000ULL, 0, rz, 0x3fd5555555555555ULL, all64, ixc,
	 "double 1/3 under round to zero"},
	{fp_sqrt_f32, 0x40000000, 0, 0, rn, 0x3fb504f3, all32, ixc, "sqrt 2 under round to nearest"},
	{fp_sqrt_f32, 0x40000000, 0, 0, rp, 0x3fb504f4, all32, ixc, "sqrt 2 under round up"},
	{fp_sqrt_f32, 0x40000000, 0, 0, rz, 0x3fb504f3, all32, ixc, "sqrt 2 under round to zero"},
	/* The cumulative exception flags. */
	{fp_div_f32, 0x3f800000, 0, 0, rn, 0x7f800000, all32, dzc, "1/0 is infinity and sets DZC"},
	{fp_div_f32, 0, 0, 0, rn, 0x7fc00000, all32, ioc, "0/0 is the default NaN and sets IOC"},
	{fp_sqrt_f32, 0xbf800000, 0, 0, rn, 0x7fc00000, all32, ioc, "sqrt(-1) is the default NaN and sets IOC"},
	{fp_mul_f32, 0x7f7fffff, 0x40000000, 0, rn, 0x7f800000, all32, ofc | ixc, "FLT_MAX*2 overflows to infinity (OFC, IXC)"},
	{fp_mul_f32, 0x7f7fffff, 0x40000000, 0, rz, 0x7f7fffff, all32, ofc | ixc, "FLT_MAX*2 under round to zero is FLT_MAX"},
	{fp_mul_f32, 0x00800001, 0x3f000000, 0, rn, 0x00400000, all32, ufc | ixc, "a tiny inexact result sets UFC and IXC"},
	{fp_mul_f32, 0x00800001, 0x3f000000, 0, rn | fz, 0, all32, ufc, "flush to zero: the tiny result is zero and sets UFC"},
	{fp_add_f32, 0x3f800000, 0x00000001, 0, rn | fz, 0x3f800000, all32, idc, "flush to zero: a denormal input sets IDC"},
	{fp_add_f32, 0x3f800000, 0x00000001, 0, rn, 0x3f800000, all32, ixc, "without flush to zero a denormal input is used"},
	{fp_add_f32, 0x00000001, 0, 0, rn, 0x00000001, all32, 0, "VFP keeps a denormal when FZ is clear"},
	{fp_add_f32, 0x00000001, 0, 0, rn | fz, 0, all32, idc, "VFP flushes a denormal input when FZ is set"},
	/* NaNs and the default NaN mode. */
	{fp_add_f32, 0x7fc12345, 0x3f800000, 0, rn, 0x7fc12345, all32, 0, "a quiet NaN propagates its payload"},
	{fp_add_f32, 0x7fc12345, 0x3f800000, 0, rn | dn, 0x7fc00000, all32, 0, "default NaN mode gives the default NaN"},
	{fp_add_f32, 0x7f812345, 0x3f800000, 0, rn, 0x7fc12345, all32, ioc, "a signalling NaN is quietened and sets IOC"},
	{fp_add_f32, 0x7f812345, 0x3f800000, 0, rn | dn, 0x7fc00000, all32, ioc, "default NaN mode on a signalling NaN"},
	/* Fused multiply-add: one rounding. */
	{fp_fma_f32, 0x3f800800, 0x3f800800, 0xbf801000, rn, 0x33800000, all32, 0, "VFMA rounds once (2^-24 survives)"},
	/* Conversions. */
	{fp_cvtr_s32_f32, 0x40200000, 0, 0, rn, 2, all32, ixc, "VCVTR 2.5 rounds to even under round to nearest"},
	{fp_cvtr_s32_f32, 0x40200000, 0, 0, rp, 3, all32, ixc, "VCVTR 2.5 under round up"},
	{fp_cvtr_s32_f32, 0x40200000, 0, 0, rm, 2, all32, ixc, "VCVTR 2.5 under round down"},
	{fp_cvtr_s32_f32, 0x40600000, 0, 0, rn, 4, all32, ixc, "VCVTR 3.5 rounds to even under round to nearest"},
	{fp_cvtr_s32_f32, 0xc0200000, 0, 0, rm, 0xfffffffd, all32, ixc, "VCVTR -2.5 under round down"},
	{fp_cvtr_s32_f32, 0xc0200000, 0, 0, rp, 0xfffffffe, all32, ixc, "VCVTR -2.5 under round up"},
	{fp_cvt_s32_f32, 0x4039999a, 0, 0, rp, 2, all32, ixc, "VCVT 2.9 truncates whatever the rounding mode"},
	{fp_cvt_s32_f32, 0xc039999a, 0, 0, rm, 0xfffffffe, all32, ixc, "VCVT -2.9 truncates toward zero"},
	{fp_cvt_s32_f32, 0x4f000000, 0, 0, rn, 0x7fffffff, all32, ioc, "VCVT 2^31 saturates and sets IOC"},
	{fp_cvt_s32_f32, 0xcf000000, 0, 0, rn, 0x80000000, all32, 0, "VCVT -2^31 is exact"},
	{fp_cvt_s32_f32, 0x7fc00000, 0, 0, rn, 0, all32, ioc, "VCVT of a NaN is zero and sets IOC"},
	{fp_cvt_u32_f32, 0xbf800000, 0, 0, rn, 0, all32, ioc, "VCVT.U32 of -1.0 is zero and sets IOC"},
	{fp_cvt_u32_f32, 0x4f800000, 0, 0, rn, 0xffffffff, all32, ioc, "VCVT.U32 of 2^32 saturates and sets IOC"},
	{fp_cvtb_f16_f32, 0x47800000, 0, 0, rn, 0x7c00, low16, ofc | ixc, "f16 of 65536 is infinity (IEEE)"},
	{fp_cvtb_f16_f32, 0x47800000, 0, 0, rn | ahp, 0x7c00, low16, 0, "f16 of 65536 is exact under AHP (exponent 31 is a number)"},
	{fp_cvtb_f16_f32, 0x48000000, 0, 0, rn | ahp, 0x7fff, low16, ioc, "f16 of 131072 saturates under AHP and sets IOC"},
	{fp_cvtb_f16_f32, 0x7f800000, 0, 0, rn | ahp, 0x7fff, low16, ioc, "f16 of infinity saturates under AHP and sets IOC"},
	{fp_cvtb_f16_f32, 0x7fc00000, 0, 0, rn | ahp, 0, low16, ioc, "f16 of a NaN is zero under AHP and sets IOC"},
	{fp_cvtb_f16_f32, 0x3f800000, 0, 0, rn, 0x3c00, low16, 0, "f16 of 1.0"},
	{fp_cvtb_f16_f32, 0x477fe000, 0, 0, rn, 0x7bff, low16, 0, "f16 of 65504"},
	{fp_cvtb_f32_f16, 0x7c00, 0, 0, rn, 0x7f800000, all32, 0, "f32 of the f16 0x7c00 is infinity (IEEE)"},
	{fp_cvtb_f32_f16, 0x7c00, 0, 0, rn | ahp, 0x47800000, all32, 0, "f32 of the AHP half 0x7c00 is 65536"},
	{fp_cvtb_f32_f16, 0x7fff, 0, 0, rn | ahp, 0x47ffe000, all32, 0, "f32 of the AHP half 0x7fff is 131008"},
	{fp_vmla_f32, 0x3f800800, 0x3f800800, 0xbf801000, rn, 0, all32, ixc, "VMLA rounds the product first (a chained multiply-add)"},
	{fp_vcvt_f32_s32, 16777217, 0, 0, rn, 0x4b800000, all32, ixc, "VCVT.F32.S32 of 2^24+1 rounds to even"},
	{fp_vcvt_f32_s32, 0xffffffff, 0, 0, rn, 0xbf800000, all32, 0, "VCVT.F32.S32 of -1 is -1.0"},
	/* Advanced SIMD floating point: the Standard FPSCR value, whatever FPSCR says. */
	{neon_mul_f32, 0x7fc123453f800001ULL, 0x3f8000003fc00000ULL, 0, rz, 0x7fc000003fc00002ULL, all64, ixc,
	 "NEON multiplies to nearest even and makes the default NaN under round to zero and no DN"},
	{neon_add_f32, 0x3f80000000000001ULL, 0, 0, rn, 0x3f80000000000000ULL, all64, idc,
	 "NEON flushes a denormal input to zero with FZ clear"},
	{neon_qadd_s8, 0x6464646464646464ULL, 0x6464646464646464ULL, 0, rn, 0x7f7f7f7f7f7f7f7fULL, all64, 0,
	 "VQADD.S8 saturates"},
	{neon_add_i32, 0x0000000100000002ULL, 0x0000000300000004ULL, 0, rn, 0x0000000400000006ULL, all64, 0,
	 "VADD.I32"},
};

/* The Armv8 floating-point additions (VRINTx, VCVTA/N/P/M and VMAXNM/VMINNM). */
const Op armv8_ops[] = {
	{fp_vrintn_f32, 0x40200000, 0, 0, rn, 0x40000000, all32, 0, "VRINTN 2.5 is 2.0"},
	{fp_vrintn_f32, 0x40600000, 0, 0, rn, 0x40800000, all32, 0, "VRINTN 3.5 is 4.0"},
	{fp_vrinta_f32, 0x40200000, 0, 0, rn, 0x40400000, all32, 0, "VRINTA 2.5 is 3.0"},
	{fp_vrinta_f32, 0xc0200000, 0, 0, rn, 0xc0400000, all32, 0, "VRINTA -2.5 is -3.0"},
	{fp_vrintp_f32, 0x40066666, 0, 0, rn, 0x40400000, all32, 0, "VRINTP 2.1 is 3.0"},
	{fp_vrintm_f32, 0xc0066666, 0, 0, rn, 0xc0400000, all32, 0, "VRINTM -2.1 is -3.0"},
	{fp_vrintz_f32, 0xc039999a, 0, 0, rn, 0xc0000000, all32, 0, "VRINTZ -2.9 is -2.0"},
	{fp_vrintx_f32, 0x40200000, 0, 0, rn, 0x40000000, all32, ixc, "VRINTX 2.5 is 2.0 and sets IXC"},
	{fp_vrintr_f32, 0x40066666, 0, 0, rp, 0x40400000, all32, 0, "VRINTR 2.1 under round up is 3.0 without IXC"},
	{fp_vcvta_s32_f32, 0x40200000, 0, 0, rn, 3, all32, ixc, "VCVTA 2.5 is 3"},
	{fp_vcvta_s32_f32, 0xc0200000, 0, 0, rn, 0xfffffffd, all32, ixc, "VCVTA -2.5 is -3"},
	{fp_vcvtn_s32_f32, 0x40200000, 0, 0, rz, 2, all32, ixc, "VCVTN 2.5 is 2"},
	{fp_vcvtn_s32_f32, 0x40600000, 0, 0, rz, 4, all32, ixc, "VCVTN 3.5 is 4"},
	{fp_vcvtp_s32_f32, 0x40066666, 0, 0, rz, 3, all32, ixc, "VCVTP 2.1 is 3"},
	{fp_vcvtm_s32_f32, 0xc0066666, 0, 0, rz, 0xfffffffd, all32, ixc, "VCVTM -2.1 is -3"},
	{fp_vmaxnm_f32, 0x7fc00000, 0x3f800000, 0, rn, 0x3f800000, all32, 0, "VMAXNM(NaN, 1.0) is 1.0"},
	{fp_vmaxnm_f32, 0x3f800000, 0x40000000, 0, rn, 0x40000000, all32, 0, "VMAXNM(1.0, 2.0) is 2.0"},
	{fp_vminnm_f32, 0x7fc00000, 0x40400000, 0, rn, 0x40400000, all32, 0, "VMINNM(NaN, 3.0) is 3.0"},
	{fp_vmaxnm_f32, 0x7fc00000, 0x7fc00000, 0, rn, 0x7fc00000, all32, 0, "VMAXNM(NaN, NaN) is NaN"},
};

bool run_op(const Op &op, FpCall &call, bool print)
{
	call = {};
	call.a = op.a;
	call.b = op.b;
	call.c = op.c;
	call.fpscr_in = op.fpscr;
	op.function(&call);
	const bool result_ok = (call.result & op.mask) == (op.result & op.mask);
	/* QC is part of the saturating add's contract; the other cumulative bits
	 * are the ones the case lists. */
	const bool is_qadd = streq(op.name, "VQADD.S8 saturates");
	const u32 flags_want = op.flags | (is_qadd ? qc : 0);
	const u32 flags_mask = cumulative | (is_qadd ? qc : 0);
	const bool flags_ok = (call.fpscr_out & flags_mask) == flags_want;
	const bool control_ok = (call.fpscr_out & 0x07c80000) == (op.fpscr & 0x07c80000);
	if (print && !(result_ok && flags_ok && control_ok)) {
		put("  ");
		put(op.name);
		put(": result ");
		put_hex(static_cast<u32>(call.result >> 32));
		put_hex(static_cast<u32>(call.result));
		put(" want ");
		put_hex(static_cast<u32>(op.result >> 32));
		put_hex(static_cast<u32>(op.result));
		put(" flags ");
		put_hex(call.fpscr_out & flags_mask);
		put(" want ");
		put_hex(flags_want);
		put(" control ");
		put_hex(call.fpscr_out & 0x07c80000);
		put("\n");
	}
	return result_ok && flags_ok && control_ok;
}

/* ---- alignment cases ---- */

struct Align {
	void (*routine)(word);
	u32 alignment;
	const char *name;
};

#define ALIGN_CASES(routine, alignment, text) \
	{routine##_arm, alignment, "A32 " text}, {routine##_thumb, alignment, "T32 " text}

const Align align_cases[] = {
	ALIGN_CASES(sa_vldr_s, 4, "VLDR S"),
	ALIGN_CASES(sa_vldr_d, 4, "VLDR D (word aligned is enough)"),
	ALIGN_CASES(sa_vstr_s, 4, "VSTR S"),
	ALIGN_CASES(sa_vstr_d, 4, "VSTR D (word aligned is enough)"),
	ALIGN_CASES(sa_vldm_s, 4, "VLDM S"),
	ALIGN_CASES(sa_vldm_d, 4, "VLDM D (word aligned is enough)"),
	ALIGN_CASES(sa_vstm_d, 4, "VSTM D (word aligned is enough)"),
	ALIGN_CASES(sa_vld1_a64, 8, "VLD1 with :64"),
	ALIGN_CASES(sa_vld1_a128, 16, "VLD1 with :128"),
	ALIGN_CASES(sa_vld1_a256, 32, "VLD1 with :256"),
	ALIGN_CASES(sa_vst1_a64, 8, "VST1 with :64"),
	ALIGN_CASES(sa_vld1_free, 1, "VLD1.32 without a qualifier"),
	ALIGN_CASES(sa_vld1_byte, 1, "VLD1.8 without a qualifier"),
	ALIGN_CASES(sa_vst1_free, 1, "VST1.64 without a qualifier"),
	ALIGN_CASES(sa_vld3_free, 1, "VLD3.8 without a qualifier"),
	ALIGN_CASES(sa_vld1_lane, 1, "VLD1 single lane without a qualifier"),
};

const char *labelled(char *buffer, const char *prefix, const char *text)
{
	word length = 0;
	for (const char *p = prefix; *p; ++p)
		buffer[length++] = *p;
	buffer[length++] = ':';
	buffer[length++] = ' ';
	for (const char *p = text; *p; ++p)
		buffer[length++] = *p;
	buffer[length] = 0;
	return buffer;
}


/* ---- ptrace: the VFP register set ---- */

struct UserVfp {
	u64 fpregs[32];
	u32 fpscr;
	u32 pad;
};
static_assert(sizeof(UserVfp) == 264);

enum : long {
	ptrace_traceme = 0,
	ptrace_cont = 7,
	ptrace_getregs = 12,
	ptrace_setregs = 13,
	ptrace_getvfpregs = 27,
	ptrace_setvfpregs = 28,
};

long wait_stopped(long pid, int *signal)
{
	i32 status = 0;
	const long got = sys(nr_wait4, pid, addr(&status), 0x40000000, 0);
	if (got != pid || (status & 0xff) != 0x7f)
		return -1;
	*signal = (status >> 8) & 0xff;
	return 0;
}

/* ---- structure forms alone in a block ---- */

/* Each form runs once as a block of its own between a block that loads D0-D31
 * and a block that stores them, with a reschedule before and after it (an
 * engine that hands a task its vectors lazily has them unloaded when the
 * form's block starts, and the reschedule saves only what the task wrote),
 * and once as one straight-line routine. The two must leave the same
 * registers and memory, and the form must have changed one of them. The
 * first round runs the blocks cold; the later rounds run them translated. */
struct SplitForm {
	const char *name;
	void (*split[2])(void *);
	void (*fused[2])(void *, const u64 *, u64 *);
};

const SplitForm split_forms[] = {
#define SPLIT_ENTRY(form) \
	{#form, {split_##form##_arm, split_##form##_thumb}, {fused_##form##_arm, fused_##form##_thumb}},
	SPLIT_FORMS(SPLIT_ENTRY)
#undef SPLIT_ENTRY
};

void split_block_cases()
{
	char name[160];
	simd_set_fpscr(0);
	u32 seed = 100;
	for (const SplitForm &form : split_forms) {
		for (u32 thumb = 0; thumb < 2; ++thumb) {
			u32 differ = 0, unchanged = 0;
			for (u32 round = 0; round < 3; ++round) {
				Image init;
				make_pattern(init, ++seed);
				alignas(16) u8 base[96], fused_memory[96], split_memory[96];
				for (u32 index = 0; index < sizeof(base); ++index)
					base[index] = static_cast<u8>(index * 37 + seed * 11 + 5);
				Image fused_out{}, split_out{};
				memcpy(fused_memory, base, sizeof(base));
				form.fused[thumb](fused_memory + 16, init.d, fused_out.d);
				memcpy(split_memory, base, sizeof(base));
				(thumb ? simd_load_thumb : simd_load)(init.d);
				yield();
				form.split[thumb](split_memory + 16);
				yield();
				(thumb ? simd_store_thumb : simd_store)(split_out.d);
				differ += mismatches(split_out.d, fused_out.d);
				differ += memcmp(split_memory, fused_memory, sizeof(base)) != 0;
				unchanged += mismatches(fused_out.d, init.d) == 0 && memcmp(fused_memory, base, sizeof(base)) == 0;
			}
			check_eq(differ, 0, labelled(name, form.name, thumb ? "in a block of its own (T32) matches one straight-line routine"
									  : "in a block of its own (A32) matches one straight-line routine"));
			check_eq(unchanged, 0, labelled(name, form.name, thumb ? "changes registers or memory (T32)"
									  : "changes registers or memory (A32)"));
		}
	}
}

} // namespace

void group_simd()
{
	char name[160];

	/* The S, D and Q views of one register file. */
	{
		u32 out[8] = {};
		simd_alias(out);
		static const u32 want[8] = {0x55667788, 0x11223344, 0x11223344, 0x55667788,
					    0x55667788, 0x11223344, 0x11223344, 0x55667788};
		check_mem(out, want, sizeof(want), "S0/S1 are the halves of D0 and Q views alias their D registers");
		u32 stack[8] = {};
		simd_pushpop(stack);
		check_eq(stack[0], 0x00020001, "VPUSH/VPOP: d8 low word");
		check_eq(stack[1], 0x00040003, "VPUSH/VPOP: d8 high word");
		check_eq(stack[2], 0x00040003, "VPUSH/VPOP: d9 low word");
		check_eq(stack[3], 0x00020001, "VPUSH/VPOP: d9 high word");
		check_eq(stack[4], 0x00020001, "VPUSH/VPOP: d10 low word");
		check_eq(stack[5], 0x00020001, "VPUSH/VPOP: d10 high word");
		check_eq(stack[6], 24, "VPUSH of three D registers moves sp by 24");
		check_eq(stack[7], 0, "VPOP restores sp");
	}

	/* The FPSCR: its writable bits round trip, and Len, Stride and the trap
	 * enables are RAZ/WI (MVFR0.FPTrap = 0). */
	simd_set_fpscr(0);
	check_eq(simd_get_fpscr(), 0, "FPSCR reads zero after writing zero");
	simd_set_fpscr(0x07c00000);
	check_eq(simd_get_fpscr() & 0x07c00000, 0x07c00000, "AHP, DN, FZ and RMode round trip");
	simd_set_fpscr(0xf0000000);
	check_eq(simd_get_fpscr() & 0xf0000000, 0xf0000000, "FPSCR.NZCV is writable by VMSR");
	simd_set_fpscr(0x0800009f);
	check_eq(simd_get_fpscr() & 0x0800009f, 0x0800009f, "QC and the cumulative flags round trip");
	simd_set_fpscr(0xffffffff);
	const u32 all_ones = simd_get_fpscr();
	if (!under_qemu()) {
		check_eq(all_ones & 0x00370000, 0, "FPSCR.Len and Stride are RAZ/WI");
		check_eq(all_ones & 0x00009f00, 0, "FPSCR trap enables are RAZ/WI");
		check_eq(all_ones & 0xf7c8009f, 0xf7c8009f, "all writable FPSCR bits are set by writing ones");
	}
	simd_set_fpscr(0);

	/* Len and Stride do not make a VFP operation a vector operation. */
	{
		Image pattern;
		make_pattern(pattern, 7);
		simd_load(pattern.d);
		FpCall call{};
		call.a = 0x3f800000;
		call.b = 0x40000000;
		call.fpscr_in = 0x00370000;
		fp_add_f32(&call);
		Image after{};
		simd_store(after.d);
		bool untouched = true;
		for (u32 index = 2; index < 6; ++index)
			untouched = untouched && after.d[index] == pattern.d[index];
		check(untouched, "with Len and Stride written, VADD.F32 changes one register only");
		check_eq(static_cast<u32>(call.result), 0x40400000, "and adds 1.0 + 2.0");
		simd_set_fpscr(0);
	}

	/* No trap: an unmasked divide by zero sets the flag and carries on. */
	{
		catch_signal(sigfpe, 4);
		FpCall call{};
		call.a = 0x3f800000;
		call.b = 0;
		call.fpscr_in = 0x00009f00;
		fp_div_f32(&call);
		release_signal(sigfpe);
		check_eq(caught.count, 0, "a divide by zero with the trap enables written raises no SIGFPE");
		check_eq(static_cast<u32>(call.result), 0x7f800000, "and gives infinity");
		check_eq(call.fpscr_out & cumulative, dzc, "and sets DZC");
		simd_set_fpscr(0);
	}

	/* The arithmetic cases. */
	for (const Op &op : ops) {
		FpCall call;
		check(run_op(op, call, true), op.name);
	}
	simd_set_fpscr(0);

	/* Compares: the flags in FPSCR, and copied into APSR by VMRS APSR_nzcv. */
	{
		struct Compare {
			void (*function)(FpCall *);
			u64 a, b;
			u32 nzcv;
			u32 flags;
			const char *name;
		};
		static const Compare compares[] = {
			{fp_cmp_f32, 0x3f800000, 0x40000000, 0x8, 0, "VCMP less"},
			{fp_cmp_f32, 0x40000000, 0x40000000, 0x6, 0, "VCMP equal"},
			{fp_cmp_f32, 0x40000000, 0x3f800000, 0x2, 0, "VCMP greater"},
			{fp_cmp_f32, 0x7fc00000, 0x3f800000, 0x3, 0, "VCMP of a quiet NaN is unordered and raises nothing"},
			{fp_cmpe_f32, 0x7fc00000, 0x3f800000, 0x3, ioc, "VCMPE of a quiet NaN sets IOC"},
			{fp_cmp_f32, 0x7f800001, 0x3f800000, 0x3, ioc, "VCMP of a signalling NaN sets IOC"},
			{fp_cmp_f64, 0x3ff0000000000000ULL, 0x4000000000000000ULL, 0x8, 0, "VCMP.F64 less"},
			{fp_cmp_f64, 0x4000000000000000ULL, 0x4000000000000000ULL, 0x6, 0, "VCMP.F64 equal"},
			{fp_cmp_f64, 0x7ff8000000000000ULL, 0x3ff0000000000000ULL, 0x3, 0, "VCMP.F64 unordered"},
		};
		for (const Compare &compare : compares) {
			FpCall call{};
			call.a = compare.a;
			call.b = compare.b;
			compare.function(&call);
			check(((call.fpscr_out >> 28) == compare.nzcv) && (call.fpscr_out & cumulative) == compare.flags,
			      compare.name);
		}
		FpCall call{};
		call.a = 0x3f800000;
		call.b = 0x40000000;
		fp_cmp_apsr_f32(&call);
		check_eq(call.result & 0xf0000000, 0x80000000, "VMRS APSR_nzcv copies the compare flags to APSR (N)");
		check_eq(call.fpscr_out >> 28, 0x8, "and leaves them in FPSCR");
		simd_set_fpscr(0);
	}

	/* The 32 registers survive system calls, rescheduling, fork and threads. */
	Image mine;
	make_pattern(mine, 1);
	const u32 mine_fpscr = 0x6bc00090;
	simd_load(mine.d);
	simd_set_fpscr(mine_fpscr);
	check_eq(simd_get_fpscr() & 0xffc8009f, mine_fpscr, "FPSCR holds a full value (NZCV, QC, DN, FZ, RZ, flags)");
	check_eq(hold_image(mine, mine_fpscr, 200), 0, "D0-D31 and FPSCR survive 200 system calls and reschedules");
	{
		/* The T32 loader and storer agree with the A32 ones. */
		Image seen{};
		simd_store_thumb(seen.d);
		check_eq(mismatches(seen.d, mine.d), 0, "T32 VSTM sees the same registers");
		Image other;
		make_pattern(other, 9);
		simd_load_thumb(other.d);
		check_eq(image_mismatches(other), 0, "and T32 VLDM loads what A32 VSTM reads");
		simd_load(mine.d);
	}

	{
		const long child = fork_child();
		if (child == 0) {
			/* The child starts with the parent's registers and has its own afterwards. */
			u32 bad = image_mismatches(mine) ? 1 : 0;
			if ((simd_get_fpscr() & 0xffc8009f) != mine_fpscr)
				++bad;
			Image own;
			make_pattern(own, 2);
			simd_load(own.d);
			simd_set_fpscr(0x03000000);
			bad += hold_image(own, 0x03000000, 200);
			quit(bad > 100 ? 100 : bad);
		}
		const long status = wait_child(child);
		check(exited_with(status, 0), "a forked child inherits the registers and keeps its own");
		check_eq(image_mismatches(mine), 0, "the parent's D0-D31 are unchanged by the child");
		check_eq(simd_get_fpscr() & 0xffc8009f, mine_fpscr, "and so is its FPSCR");
	}

	{
		struct ThreadState {
			u32 seed;
			u32 bad;
		};
		static ThreadState states[3];
		auto body = [](void *argument) -> long {
			auto *state = static_cast<ThreadState *>(argument);
			Image own;
			make_pattern(own, state->seed);
			simd_load(own.d);
			const u32 fpscr = 0x01000000U | (state->seed << 22);
			simd_set_fpscr(fpscr & 0x07c00000);
			state->bad = hold_image(own, fpscr & 0x07c00000, 300);
			return 0;
		};
		Thread threads[3];
		bool started = true;
		for (u32 index = 0; index < 3; ++index) {
			states[index] = {20 + index, 0};
			started = started && thread_start(threads[index], body, &states[index]);
		}
		check(started, "clone starts three threads");
		bool joined = started;
		for (u32 index = 0; index < 3 && started; ++index)
			joined = thread_join(threads[index], 120) && joined;
		check(joined, "the threads finish");
		check_eq(states[0].bad + states[1].bad + states[2].bad, 0,
			 "three threads each keep their own D0-D31 and FPSCR");
		check_eq(image_mismatches(mine), 0, "and the first thread's registers are unchanged");
		check_eq(simd_get_fpscr() & 0xffc8009f, mine_fpscr, "and its FPSCR");
	}

	/* Signals: the frame carries D0-D31 and FPSCR; the handler may change
	 * them; sigreturn restores the rest. */
	{
		handler_result = {};
		handler_result.expect = mine;
		handler_result.expect_fpscr = mine_fpscr;
		handler_result.modify = true;
		handler_result.new_d3 = 0xfeedfacecafebeefULL;
		handler_result.new_fpscr = 0x92400080;
		set_handler(sigusr1, frame_handler);
		raise_signal(sigusr1);
		reset_handler(sigusr1);
		check_eq(handler_result.deliveries, 1, "the signal was delivered once");
		check_eq(handler_result.magic_ok, 1, "the frame has the VFP magic 0x56465001");
		check_eq(handler_result.size_ok, 1, "and the VFP frame size");
		check_eq(handler_result.image_bad, 0, "the frame holds D0-D31 as they were");
		check_eq(handler_result.fpscr_frame_bad, 0, "and FPSCR as it was");
		Image after{};
		simd_store(after.d);
		Image want = mine;
		want.d[3] = 0xfeedfacecafebeefULL;
		check_eq(mismatches(after.d, want.d), 0,
			 "sigreturn restores D0-D31 and takes the register the handler changed in the frame");
		check_eq(simd_get_fpscr() & 0xffc8009f, 0x92400080, "and takes the FPSCR the handler wrote in the frame");
		simd_load(mine.d);
		simd_set_fpscr(mine_fpscr);
	}
	{
		/* With the handler changing nothing, everything comes back as it was. */
		handler_result = {};
		handler_result.expect = mine;
		handler_result.expect_fpscr = mine_fpscr;
		set_handler(sigusr2, frame_handler);
		raise_signal(sigusr2);
		reset_handler(sigusr2);
		Image after{};
		simd_store(after.d);
		check_eq(mismatches(after.d, mine.d), 0, "a handler that clobbers every D register leaves no trace");
		check_eq(simd_get_fpscr() & 0xffc8009f, mine_fpscr, "or in FPSCR");
	}
	{
		/* Asynchronous signals in the middle of a VFP and NEON loop. */
		handler_result = {};
		set_handler(sigalrm, alarm_handler);
		struct {
			long interval_seconds, interval_microseconds, value_seconds, value_microseconds;
		} timer{0, 1000, 0, 1000};
		sys(nr_setitimer, 0, addr(&timer), 0);
		constexpr u32 count = 1500000;
		u32 out[12] = {};
		simd_set_fpscr(0);
		simd_spin(count, out);
		timer = {0, 0, 0, 0};
		sys(nr_setitimer, 0, addr(&timer), 0);
		reset_handler(sigalrm);
		const u64 want = double_bits(count);
		check_eq64(static_cast<u64>(out[0]) | (static_cast<u64>(out[1]) << 32), want,
			   "a VFP loop interrupted by timer signals still adds exactly");
		check(out[2] == count && out[3] == count && out[4] == count && out[5] == count,
		      "a NEON loop interrupted by timer signals still adds exactly");
		check(out[6] == 0x5a && out[7] == 0x5a && out[8] == 0x5a && out[9] == 0x5a,
		      "and a register the loop never writes keeps its value");
		check(out[10] == 0x56781234 && out[11] == 0x56781234,
		      "and so does a D register from the middle of the bank");
		check(handler_result.deliveries >= 3, "at least three timer signals arrived during the loop");
		check_eq(handler_result.magic_ok, handler_result.deliveries, "each timer signal frame carries the VFP block");
		check_eq(simd_get_fpscr() & 0xffc8009f, 0, "and FPSCR is as the loop had it");
	}

	/* FPSCR flags and compare results survive a system call and a signal. */
	{
		FpCall call{};
		call.a = 0x3f800000;
		call.b = 0x40000000;
		fp_cmp_f32(&call);
		const u32 before = simd_get_fpscr();
		check_eq(before >> 28, 0x8, "VCMP leaves N");
		getpid();
		yield();
		check_eq(simd_get_fpscr(), before, "the compare flags survive a system call");
		handler_result = {};
		handler_result.expect_fpscr = before & 0xffc8009f;
		Image ignore;
		simd_store(ignore.d);
		handler_result.expect = ignore;
		set_handler(sigusr1, frame_handler);
		raise_signal(sigusr1);
		reset_handler(sigusr1);
		check_eq(handler_result.fpscr_frame >> 28, 0x8, "the signal frame has the compare flags");
		check_eq(simd_get_fpscr(), before, "and the flags are back after it");
		simd_set_fpscr(0);
	}

	/* Alignment. */
	{
		const word area = mmap_pages(2 * page_size, prot_read | prot_write);
		for (u32 index = 0; index < 2 * page_size; ++index)
			ptr<u8>(area)[index] = static_cast<u8>(index * 5 + 1);
		for (const Align &fault : align_cases) {
			u32 should_have = 0, should_not = 0, bad_address = 0, bad_code = 0, bad_pc = 0;
			for (word offset = 0; offset < 40; ++offset) {
				const word address = area + 64 + offset;
				catch_signal(sigbus, return_to_lr);
				fault.routine(address);
				release_signal(sigbus);
				const bool faults = (offset % fault.alignment) != 0;
				if (faults) {
					if (caught.count != 1)
						++should_have;
					else {
						if (caught.address != address)
							++bad_address;
						if (caught.code != 1)
							++bad_code;
						if (caught.pc != (addr(fault.routine) & ~1UL))
							++bad_pc;
					}
				} else if (caught.count != 0) {
					++should_not;
				}
			}
			check_eq(should_have, 0, labelled(name, fault.name, "a misaligned access raises SIGBUS"));
			check_eq(should_not, 0, labelled(name, fault.name, "an access that needs no more alignment does not fault"));
			check_eq(bad_address, 0, labelled(name, fault.name, "si_addr is the access address"));
			check_eq(bad_code, 0, labelled(name, fault.name, "si_code is BUS_ADRALN"));
			check_eq(bad_pc, 0, labelled(name, fault.name, "the signal pc is the instruction"));
		}
	}
	/* ptrace: the tracer reads and writes the tracee's VFP registers. */
	if (!under_qemu()) {
		Image pattern;
		make_pattern(pattern, 5);
		auto *report = ptr<Image>(mmap_pages(4096, prot_read | prot_write, map_shared | map_anonymous));
		auto *report_fpscr = ptr<u32>(addr(report) + 1024);
		const long child = fork_child();
		if (child == 0) {
			sys(nr_ptrace, ptrace_traceme, 0, 0, 0);
			raise_signal(sigstop);
			simd_load(pattern.d);
			simd_set_fpscr(0x42400080);
			simd_bkpt();
			simd_store(report->d);
			*report_fpscr = simd_get_fpscr();
			quit(0);
		}
		int signal = 0;
		bool ok = wait_stopped(child, &signal) == 0 && signal == sigstop;
		check(ok, "a traced child stops at SIGSTOP");
		if (ok) {
			sys(nr_ptrace, ptrace_cont, child, 0, 0);
			ok = wait_stopped(child, &signal) == 0 && signal == sigtrap;
			check(ok, "and at its BKPT with the registers loaded");
		}
		if (ok) {
			UserVfp vfp{};
			check_eq(static_cast<u32>(sys(nr_ptrace, ptrace_getvfpregs, child, 0, addr(&vfp))), 0,
				 "PTRACE_GETVFPREGS");
			check_eq(mismatches(vfp.fpregs, pattern.d), 0, "returns D0-D31 of the tracee");
			check_eq(vfp.fpscr & 0xffc8009f, 0x42400080, "and its FPSCR");
			vfp.fpregs[5] = 0x0123456789abcdefULL;
			vfp.fpscr = 0x03000000;
			check_eq(static_cast<u32>(sys(nr_ptrace, ptrace_setvfpregs, child, 0, addr(&vfp))), 0,
				 "PTRACE_SETVFPREGS");
			u32 regs[18] = {};
			sys(nr_ptrace, ptrace_getregs, child, 0, addr(regs));
			regs[15] += 4;
			sys(nr_ptrace, ptrace_setregs, child, 0, addr(regs));
			sys(nr_ptrace, ptrace_cont, child, 0, 0);
		}
		const long status = wait_child(child);
		check(exited_with(status, 0), "the tracee ran on");
		Image want = pattern;
		want.d[5] = 0x0123456789abcdefULL;
		check_eq(mismatches(report->d, want.d), 0, "and sees the register the tracer wrote");
		check_eq(*report_fpscr & 0xffc8009f, 0x03000000, "and the FPSCR the tracer wrote");
	}

	/* Structure loads and stores alone in a block publish their registers. */
	split_block_cases();
}

/* The Armv8 floating-point additions against known answers. */
void group_armv8_fp()
{
	for (const Op &op : armv8_ops) {
		FpCall call;
		check(run_op(op, call, true), op.name);
	}
}

} // namespace probe
