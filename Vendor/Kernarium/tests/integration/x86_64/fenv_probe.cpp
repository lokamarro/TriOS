/* SPDX-License-Identifier: GPL-2.0-or-later */
/* What software that reads x86's sticky floating-point exception flags
 * sees, through glibc, with every exception masked.
 *
 *   fenv-probe flags    one line per operation: fetestexcept() and MXCSR
 *   fenv-probe narrow   glibc's fadd, which reads the precision flag to
 *                       round to odd
 *
 * Operands come from volatile storage so that nothing folds at compile
 * time. Build in the guest with `g++ -std=c++20 -O2 fenv_probe.cpp`. */
#include <cfenv>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <math.h>
#include <xmmintrin.h>

namespace {

volatile double D[8] = {1.0, 0.0, -1.0, 3.0, 1000.0, -1000.0, 0.5, 1e22};
volatile float F[4] = {3.0f, 0.0f, 2.0f, 1.0f};
volatile double QN = __builtin_nan("");
volatile double sink;
volatile float sinkf;
volatile int sinki;
volatile long double LA = 1.0L, LB = 3.0L, LBIG = 1e22L;
volatile long double sinkl;

double dmax(void) { return D[0] * DBL_MAX; }
double dmin(void) { return D[0] * DBL_MIN; }

void op_div0(void) { sink = D[0] / D[1]; }
void op_inv(void) { sink = D[1] / D[1]; }
void op_sqrtneg(void) { sink = __builtin_sqrt(D[2]); }
void op_ovf(void) { sink = dmax() * 2.0; }
void op_unf(void) { sink = dmin() * dmin(); }
void op_inexact(void) { sink = D[0] / D[3]; }
void op_exact(void) { sink = D[0] + D[0]; }
void op_denorm(void) { sink = (dmin() / 4.0) * D[0]; }
void op_cvtnan(void) { sinki = static_cast<int>(QN); }
void op_cvtbig(void) { sinki = static_cast<int>(D[7]); }
void op_cmpnan(void) { double n = QN; sinki = n < D[0]; }
void op_eqnan(void) { double n = QN; sinki = n == D[0]; }
void op_fltovf(void) { sinkf = F[0] * FLT_MAX; }
void op_rcp(void) { sinkf = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(F[0]))); }
void op_log0(void) { sink = log(D[1]); }
void op_expbig(void) { sink = exp(D[4]); }
void op_expsmall(void) { sink = exp(D[5]); }
void op_powneg(void) { sink = pow(D[2], D[6]); }
void op_sinbig(void) { sink = sin(D[7]); }
void op_raisediv(void) { feraiseexcept(FE_DIVBYZERO); }
void op_raiseinv(void) { feraiseexcept(FE_INVALID); }
void op_raiseovf(void) { feraiseexcept(FE_OVERFLOW); }
void op_nearbyint(void) { sink = nearbyint(D[6] + D[0]); }
void op_lrint(void) { sinki = static_cast<int>(lrint(D[6] + D[0])); }
void op_rint(void) { sink = rint(D[6] + D[0]); }
/* x87 arithmetic raises flags in the x87 status word only, not in MXCSR. */
void op_x87div(void) { sinkl = LA / LB; }
void op_x87sin(void) { sinkl = sinl(LBIG); }
/* Under FTZ a tiny result flushes with UE and PE; under DAZ a denormal
 * input reads as zero and raises no DE. Each restores MXCSR's controls. */
void op_ftzflush(void)
{
	unsigned m = _mm_getcsr();
	_mm_setcsr(m | 0x8000);
	sink = dmin() * D[6];
	_mm_setcsr((_mm_getcsr() & 0x3fU) | m);
}
void op_dazdenorm(void)
{
	unsigned m = _mm_getcsr();
	double d = dmin() / 4.0;
	_mm_setcsr((m | 0x40) & ~0x3fU);
	sink = d * D[0];
	_mm_setcsr((_mm_getcsr() & 0x3fU) | m);
}

const struct {
	const char *name;
	void (*fn)(void);
} cases[] = {
	{"div0", op_div0}, {"inv", op_inv}, {"sqrtneg", op_sqrtneg},
	{"ovf", op_ovf}, {"unf", op_unf}, {"inexact", op_inexact},
	{"exact", op_exact}, {"denorm", op_denorm}, {"cvtnan", op_cvtnan},
	{"cvtbig", op_cvtbig}, {"cmpnan", op_cmpnan}, {"eqnan", op_eqnan},
	{"fltovf", op_fltovf}, {"rcp", op_rcp}, {"log0", op_log0},
	{"expbig", op_expbig}, {"expsmall", op_expsmall}, {"powneg", op_powneg},
	{"sinbig", op_sinbig}, {"raisediv", op_raisediv},
	{"raiseinv", op_raiseinv}, {"raiseovf", op_raiseovf},
	{"nearbyint", op_nearbyint}, {"lrint", op_lrint}, {"rint", op_rint},
	{"x87div", op_x87div}, {"x87sin", op_x87sin},
	{"ftzflush", op_ftzflush}, {"dazdenorm", op_dazdenorm},
};

void flag_names(int flags, char *out)
{
	out[0] = 0;
	if (flags & FE_INVALID)
		strcat(out, "IE ");
	if (flags & FE_DIVBYZERO)
		strcat(out, "ZE ");
	if (flags & FE_OVERFLOW)
		strcat(out, "OE ");
	if (flags & FE_UNDERFLOW)
		strcat(out, "UE ");
	if (flags & FE_INEXACT)
		strcat(out, "PE ");
	if (!out[0])
		strcpy(out, "- ");
}

} // namespace

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "flags";
	if (!strcmp(mode, "narrow")) {
		/* 1 + 2^-24 + 2^-80 lies above a float tie, so x86 rounds it up:
		 * fadd sums toward zero, then sets the last bit if PE is set. */
		volatile double x = 1.0 + 0x1p-24, y = 0x1p-80;
		float r = fadd(x, y);
		unsigned u;
		memcpy(&u, &r, 4);
		printf("fenv narrow fadd %08x\n", u);
		return 0;
	}
	if (strcmp(mode, "flags"))
		return 2;
	for (std::size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		char names[32];
		feclearexcept(FE_ALL_EXCEPT);
		_mm_setcsr(_mm_getcsr() & ~0x3fU);
		cases[i].fn();
		int flags = fetestexcept(FE_ALL_EXCEPT);
		unsigned mxcsr = _mm_getcsr();
		flag_names(flags, names);
		printf("fenv %-10s fetestexcept %-15s mxcsr %04x\n", cases[i].name,
			names, mxcsr);
	}
	return 0;
}
