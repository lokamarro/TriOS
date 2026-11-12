/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A program that picks its integer dot-product path from CPUID, the way
 * quantized-inference code does: it runs the AVX-VNNI form of its kernels
 * when CPUID leaf 7 subleaf 1 reports AVX-VNNI, and an AVX2 form built from
 * VPMADDUBSW, VPMADDWD and VPADDD otherwise. Three kernels have those two
 * forms:
 *
 *   q8_0   a block-quantized dot product with ggml's Q8_0 x Q8_0 shape (32
 *          signed bytes and an FP16 scale per block, the VPSIGNB trick that
 *          turns a signed-by-signed product into unsigned-by-signed, FMA
 *          accumulation across blocks),
 *   gemm   an unsigned-by-signed 8-bit GEMM with a 4x16 register tile over
 *          a B matrix packed in groups of four k, the shape of the u8s8
 *          kernels in MLAS, oneDNN and XNNPACK, and
 *   dot16  a signed 16-bit dot product with two accumulators, VPDPWSSD
 *          against VPMADDWD and VPADDD, as the s16 kernels of oneDNN and
 *          MLAS accumulate.
 *
 *   vnni-probe check [vnni|avx2]   report CPUID, run both forms of every
 *                                  kernel on the same data and compare them
 *                                  with exact integer references; with an
 *                                  argument, also require the dispatcher to
 *                                  have chosen that path
 *   vnni-probe bench ORDER ROUNDS REPS
 *                                  time the two forms in the order ORDER (V
 *                                  for AVX-VNNI, A for AVX2, such as VAAV),
 *                                  repeated ROUNDS times, REPS calls per
 *                                  batch, with a plain FMA kernel timed
 *                                  before and after as a same-run control
 *
 * Built freestanding for x86-64 (probe_runner.py). Data stays in the ranges
 * where VPMADDUBSW's 16-bit saturation cannot differ from VPDPBUSD. */
#include <immintrin.h>

namespace {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long;
using i8 = signed char;
using i16 = short;
using i32 = int;
using i64 = long;

inline long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0)
{
	long result;
	__asm__ volatile("syscall"
			 : "=a"(result)
			 : "a"(number), "D"(a0), "S"(a1), "d"(a2)
			 : "rcx", "r11", "memory", "cc");
	return result;
}

constexpr long nr_write = 1;
constexpr long nr_clock_gettime = 228;

void put(const char *text)
{
	long length = 0;
	while (text[length] != '\0')
		++length;
	raw_syscall(nr_write, 1, reinterpret_cast<long>(text), length);
}

void put_number(u64 value)
{
	char text[24];
	int position = 23;
	text[position] = '\0';
	do {
		text[--position] = static_cast<char>('0' + value % 10);
		value /= 10;
	} while (value != 0);
	put(text + position);
}

void put_hex(u64 value)
{
	char text[19];
	int position = 18;
	text[position] = '\0';
	do {
		text[--position] = "0123456789abcdef"[value & 0xf];
		value >>= 4;
	} while (value != 0);
	text[--position] = 'x';
	text[--position] = '0';
	put(text + position);
}

bool equal(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		++left;
		++right;
	}
	return *left == *right;
}

u64 now_ns()
{
	struct {
		long seconds;
		long nanoseconds;
	} time{};
	raw_syscall(nr_clock_gettime, 1, reinterpret_cast<long>(&time));
	return static_cast<u64>(time.seconds) * 1000000000UL +
	       static_cast<u64>(time.nanoseconds);
}

u32 random_state = 0x2545f491U;

u32 random_word()
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/* ---- CPUID ---------------------------------------------------------- */

struct Cpuid {
	u32 eax, ebx, ecx, edx;
};

Cpuid cpuid(u32 leaf, u32 subleaf)
{
	Cpuid result;
	__asm__ volatile("cpuid"
			 : "=a"(result.eax), "=b"(result.ebx), "=c"(result.ecx),
			   "=d"(result.edx)
			 : "a"(leaf), "c"(subleaf));
	return result;
}

u64 xgetbv0()
{
	u32 low, high;
	__asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
	return (static_cast<u64>(high) << 32) | low;
}

struct Features {
	u32 max_subleaf;
	bool avx2;
	bool avx_vnni;
};

/* The checks a library makes before it takes the AVX-VNNI path: the
 * operating system enables YMM state, leaf 7 exists and has the AVX2 bit,
 * and subleaf 1 is enumerated (leaf 7 subleaf 0 EAX is the highest subleaf)
 * and reports AVX-VNNI in EAX bit 4. */
Features detect()
{
	Features features{};
	if (cpuid(0, 0).eax < 7)
		return features;
	const Cpuid one = cpuid(1, 0);
	const bool osxsave = (one.ecx >> 27 & 1) != 0;
	const bool avx = (one.ecx >> 28 & 1) != 0;
	if (!osxsave || !avx || (xgetbv0() & 6) != 6)
		return features;
	const Cpuid seven = cpuid(7, 0);
	features.max_subleaf = seven.eax;
	features.avx2 = (seven.ebx >> 5 & 1) != 0;
	if (features.avx2 && seven.eax >= 1)
		features.avx_vnni = (cpuid(7, 1).eax >> 4 & 1) != 0;
	return features;
}

/* ---- data ----------------------------------------------------------- */

constexpr int q8_block = 32;
constexpr int q8_blocks = 512;
constexpr int q8_rows = 16;

struct Q8Block {
	u16 scale; /* IEEE binary16 */
	i8 values[q8_block];
} __attribute__((packed));

/* The scales are 1.0, 0.5 and 0.25 as binary16, so every product of two
 * scales is exact. */
constexpr u16 scales[3] = {0x3c00, 0x3800, 0x3400};

Q8Block weights[q8_rows][q8_blocks];
Q8Block activations[q8_blocks];
float q8_results[2][q8_rows];

constexpr int gemm_m = 64;
constexpr int gemm_n = 64;
constexpr int gemm_k = 256;

u8 gemm_a[gemm_m][gemm_k];
/* B packed as [k / 4][n][4]: the four k values of one column are adjacent,
 * which is what a dot-product instruction consumes. */
i8 gemm_b[gemm_k / 4][gemm_n][4];
i32 gemm_c[3][gemm_m][gemm_n];

constexpr int dot16_length = 4096;
i16 dot16_x[dot16_length];
i16 dot16_y[dot16_length];

void fill()
{
	for (auto &row : weights)
		for (auto &block : row) {
			block.scale = scales[random_word() % 3];
			for (auto &value : block.values) {
				/* -127..127, as quantization produces. */
				value = static_cast<i8>(
					static_cast<int>(random_word() % 255) -
					127);
			}
		}
	for (auto &block : activations) {
		block.scale = scales[random_word() % 3];
		for (auto &value : block.values)
			value = static_cast<i8>(
				static_cast<int>(random_word() % 255) - 127);
	}
	/* A in 0..127 and B in -128..127: a pair of products stays within
	 * 16 bits, so VPMADDUBSW cannot saturate. */
	for (auto &row : gemm_a)
		for (auto &value : row)
			value = static_cast<u8>(random_word() % 128);
	for (auto &group : gemm_b)
		for (auto &column : group)
			for (auto &value : column)
				value = static_cast<i8>(
					static_cast<int>(random_word() % 256) -
					128);
	/* Small enough that no lane's sum can overflow 32 bits. */
	for (auto &value : dot16_x)
		value = static_cast<i16>(static_cast<int>(random_word() % 2048) - 1024);
	for (auto &value : dot16_y)
		value = static_cast<i16>(static_cast<int>(random_word() % 2048) - 1024);
}

/* ---- the two forms of each kernel ---------------------------------- */

/* The x86-64 JIT runs a 256-bit loop at a speed that depends on how the
 * addresses of the loop's operand arrays are aligned, by up to a factor of two
 * in either direction and for either instruction form; the same loop in
 * 128-bit instructions, and the gadget engine, do not show it. Building with
 * -DVNNI_PROBE_PAD=N moves every kernel and, behind the code, the data by N
 * bytes (a multiple of 16), so a comparison can be made at several
 * placements. */
#ifndef VNNI_PROBE_PAD
#define VNNI_PROBE_PAD 0
#endif
#define VNNI_PROBE_STRING_(text) #text
#define VNNI_PROBE_STRING(text) VNNI_PROBE_STRING_(text)
__asm__(".text\n\t.p2align 4\n\t.skip " VNNI_PROBE_STRING(VNNI_PROBE_PAD) ", 0x90\n");

#define AVX2_TARGET __attribute__((target("avx2,fma,f16c")))
#define VNNI_TARGET __attribute__((target("avx2,fma,f16c,avxvnni")))

/* ggml's mul_sum_i8_pairs_float: the signed product through the unsigned
 * form, summed in groups of four into eight 32-bit lanes. */
AVX2_TARGET inline __m256 sum_pairs_avx2(__m256i qx, __m256i qy)
{
	const __m256i ax = _mm256_sign_epi8(qx, qx);
	const __m256i sy = _mm256_sign_epi8(qy, qx);
	const __m256i dot = _mm256_maddubs_epi16(ax, sy);
	const __m256i ones = _mm256_set1_epi16(1);
	return _mm256_cvtepi32_ps(_mm256_madd_epi16(ones, dot));
}

VNNI_TARGET inline __m256 sum_pairs_vnni(__m256i qx, __m256i qy)
{
	const __m256i ax = _mm256_sign_epi8(qx, qx);
	const __m256i sy = _mm256_sign_epi8(qy, qx);
	return _mm256_cvtepi32_ps(
		_mm256_dpbusd_avx_epi32(_mm256_setzero_si256(), ax, sy));
}

AVX2_TARGET inline float horizontal_sum(__m256 sum)
{
	__m128 low = _mm_add_ps(_mm256_castps256_ps128(sum),
				_mm256_extractf128_ps(sum, 1));
	low = _mm_add_ps(low, _mm_movehl_ps(low, low));
	low = _mm_add_ss(low, _mm_movehdup_ps(low));
	return _mm_cvtss_f32(low);
}

#define DEFINE_Q8_DOT(NAME, TARGET, SUM_PAIRS)                               \
	TARGET float NAME(const Q8Block *x, const Q8Block *y, int blocks)    \
	{                                                                    \
		__m256 accumulator = _mm256_setzero_ps();                    \
		for (int block = 0; block < blocks; ++block) {               \
			const __m256 scale = _mm256_set1_ps(                 \
				_cvtsh_ss(x[block].scale) *                  \
				_cvtsh_ss(y[block].scale));                  \
			const __m256i qx = _mm256_loadu_si256(               \
				reinterpret_cast<const __m256i *>(           \
					x[block].values));                   \
			const __m256i qy = _mm256_loadu_si256(               \
				reinterpret_cast<const __m256i *>(           \
					y[block].values));                   \
			accumulator = _mm256_fmadd_ps(                       \
				scale, SUM_PAIRS(qx, qy), accumulator);      \
		}                                                            \
		return horizontal_sum(accumulator);                          \
	}

DEFINE_Q8_DOT(q8_dot_avx2, AVX2_TARGET, sum_pairs_avx2)
DEFINE_Q8_DOT(q8_dot_vnni, VNNI_TARGET, sum_pairs_vnni)

/* One 4 x 16 tile of C over the whole of K. */
#define DEFINE_GEMM(NAME, TARGET, ACCUMULATE)                                \
	TARGET void NAME(i32 (*c)[gemm_n])                                   \
	{                                                                    \
		const __m256i ones = _mm256_set1_epi16(1);                   \
		(void)ones;                                                  \
		for (int n0 = 0; n0 < gemm_n; n0 += 16) {                    \
			for (int m0 = 0; m0 < gemm_m; m0 += 4) {             \
				__m256i acc[4][2];                           \
				for (auto &row : acc)                        \
					for (auto &lane : row)               \
						lane = _mm256_setzero_si256();\
				for (int k4 = 0; k4 < gemm_k / 4; ++k4) {    \
					const __m256i b0 = _mm256_loadu_si256( \
						reinterpret_cast<const __m256i *>( \
							&gemm_b[k4][n0][0]));  \
					const __m256i b1 = _mm256_loadu_si256( \
						reinterpret_cast<const __m256i *>( \
							&gemm_b[k4][n0 + 8][0]));\
					for (int m = 0; m < 4; ++m) {        \
						const __m256i a = _mm256_set1_epi32( \
							*reinterpret_cast<const i32 *>( \
								&gemm_a[m0 + m][k4 * 4])); \
						ACCUMULATE(acc[m][0], a, b0);\
						ACCUMULATE(acc[m][1], a, b1);\
					}                                    \
				}                                            \
				for (int m = 0; m < 4; ++m) {                \
					_mm256_storeu_si256(                 \
						reinterpret_cast<__m256i *>( \
							&c[m0 + m][n0]),     \
						acc[m][0]);                  \
					_mm256_storeu_si256(                 \
						reinterpret_cast<__m256i *>( \
							&c[m0 + m][n0 + 8]), \
						acc[m][1]);                  \
				}                                            \
			}                                                    \
		}                                                            \
	}

#define ACCUMULATE_AVX2(ACC, A, B)                                           \
	(ACC) = _mm256_add_epi32(                                            \
		(ACC), _mm256_madd_epi16(_mm256_maddubs_epi16((A), (B)), ones))
#define ACCUMULATE_VNNI(ACC, A, B) (ACC) = _mm256_dpbusd_avx_epi32((ACC), (A), (B))

DEFINE_GEMM(gemm_avx2, AVX2_TARGET, ACCUMULATE_AVX2)
DEFINE_GEMM(gemm_vnni, VNNI_TARGET, ACCUMULATE_VNNI)

AVX2_TARGET inline i32 horizontal_sum_epi32(__m256i sum)
{
	__m128i low = _mm_add_epi32(_mm256_castsi256_si128(sum),
				    _mm256_extracti128_si256(sum, 1));
	low = _mm_add_epi32(low, _mm_shuffle_epi32(low, 0x4e));
	low = _mm_add_epi32(low, _mm_shuffle_epi32(low, 0xb1));
	return _mm_cvtsi128_si32(low);
}

#define ACCUMULATE16_AVX2(ACC, A, B)                                         \
	(ACC) = _mm256_add_epi32((ACC), _mm256_madd_epi16((A), (B)))
#define ACCUMULATE16_VNNI(ACC, A, B)                                         \
	(ACC) = _mm256_dpwssd_avx_epi32((ACC), (A), (B))

#define DEFINE_DOT16(NAME, TARGET, ACCUMULATE)                               \
	TARGET i32 NAME()                                                    \
	{                                                                    \
		__m256i acc0 = _mm256_setzero_si256();                       \
		__m256i acc1 = _mm256_setzero_si256();                       \
		for (int i = 0; i < dot16_length; i += 32) {                 \
			const auto x0 = _mm256_loadu_si256(                  \
				reinterpret_cast<const __m256i *>(&dot16_x[i])); \
			const auto y0 = _mm256_loadu_si256(                  \
				reinterpret_cast<const __m256i *>(&dot16_y[i])); \
			const auto x1 = _mm256_loadu_si256(                  \
				reinterpret_cast<const __m256i *>(&dot16_x[i + 16])); \
			const auto y1 = _mm256_loadu_si256(                  \
				reinterpret_cast<const __m256i *>(&dot16_y[i + 16])); \
			ACCUMULATE(acc0, x0, y0);                            \
			ACCUMULATE(acc1, x1, y1);                            \
		}                                                            \
		return horizontal_sum_epi32(_mm256_add_epi32(acc0, acc1));   \
	}

DEFINE_DOT16(dot16_avx2, AVX2_TARGET, ACCUMULATE16_AVX2)
DEFINE_DOT16(dot16_vnni, VNNI_TARGET, ACCUMULATE16_VNNI)

/* A plain AVX2 FMA loop that neither path touches, timed before and after
 * the arms as a same-run control. */
AVX2_TARGET float control_kernel(int iterations)
{
	__m256 a = _mm256_set1_ps(1.0f), b = _mm256_set1_ps(0.5f);
	__m256 c = _mm256_set1_ps(0.25f), d = _mm256_set1_ps(0.125f);
	const __m256 m = _mm256_set1_ps(0.999f), k = _mm256_set1_ps(0.001f);
	for (int i = 0; i < iterations; ++i) {
		a = _mm256_fmadd_ps(a, m, k);
		b = _mm256_fmadd_ps(b, m, k);
		c = _mm256_fmadd_ps(c, m, k);
		d = _mm256_fmadd_ps(d, m, k);
	}
	return horizontal_sum(_mm256_add_ps(_mm256_add_ps(a, b),
					    _mm256_add_ps(c, d)));
}

/* ---- running and checking ------------------------------------------ */

enum class Path { avx2, vnni };

constexpr Path both_paths[2] = {Path::avx2, Path::vnni};

const char *path_name(Path path)
{
	return path == Path::vnni ? "vnni" : "avx2";
}

float run_q8(Path path, float *results)
{
	float total = 0;
	for (int row = 0; row < q8_rows; ++row) {
		results[row] = path == Path::vnni
			? q8_dot_vnni(weights[row], activations, q8_blocks)
			: q8_dot_avx2(weights[row], activations, q8_blocks);
		total += results[row];
	}
	return total;
}

void run_gemm(Path path, i32 (*c)[gemm_n])
{
	if (path == Path::vnni)
		gemm_vnni(c);
	else
		gemm_avx2(c);
}

i32 run_dot16(Path path)
{
	return path == Path::vnni ? dot16_vnni() : dot16_avx2();
}

u32 float_bits(float value)
{
	u32 bits;
	__builtin_memcpy(&bits, &value, sizeof bits);
	return bits;
}

/* The exact dot product of one weight row with the activations, as
 * integers: every scale is a power of two, so the products with their
 * scales are exact in double. */
double q8_reference(const Q8Block *x, const Q8Block *y, int blocks)
{
	double sum = 0;
	for (int block = 0; block < blocks; ++block) {
		i64 dot = 0;
		for (int i = 0; i < q8_block; ++i)
			dot += static_cast<i64>(x[block].values[i]) *
			       y[block].values[i];
		const double sx = x[block].scale == 0x3c00 ? 1.0
				: x[block].scale == 0x3800 ? 0.5
							   : 0.25;
		const double sy = y[block].scale == 0x3c00 ? 1.0
				: y[block].scale == 0x3800 ? 0.5
							   : 0.25;
		sum += sx * sy * static_cast<double>(dot);
	}
	return sum;
}

int check(const char *expected)
{
	int failures = 0;
	const Features features = detect();
	put("vnni-probe cpuid max-subleaf=");
	put_number(features.max_subleaf);
	put(" avx2=");
	put_number(features.avx2);
	put(" avx-vnni=");
	put_number(features.avx_vnni);
	put("\n");
	if (!features.avx2) {
		put("vnni-probe FAIL the probe needs AVX2\n");
		return 1;
	}
	const Path chosen = features.avx_vnni ? Path::vnni : Path::avx2;
	put("vnni-probe dispatch path=");
	put(path_name(chosen));
	put("\n");
	if (expected != nullptr && !equal(expected, path_name(chosen))) {
		put("vnni-probe FAIL the dispatcher chose ");
		put(path_name(chosen));
		put(", expected ");
		put(expected);
		put("\n");
		++failures;
	}

	fill();
	/* The VNNI form runs whatever CPUID says: both x86 engines execute
	 * it, and its results are the check that they do so correctly. */
	for (Path path : both_paths) {
		float *results = q8_results[path == Path::vnni];
		run_q8(path, results);
		int mismatches = 0;
		for (int row = 0; row < q8_rows; ++row) {
			const double want = q8_reference(
				weights[row], activations, q8_blocks);
			const double got = results[row];
			double difference = got - want;
			if (difference < 0)
				difference = -difference;
			double tolerance = want < 0 ? -want : want;
			tolerance = tolerance * 1e-5 + 1e-3;
			if (!(difference <= tolerance))
				++mismatches;
		}
		put("vnni-probe q8_0 path=");
		put(path_name(path));
		put(" row0=");
		put_hex(float_bits(results[0]));
		put(mismatches == 0 ? " ok\n" : " FAIL\n");
		failures += mismatches != 0;
	}
	for (int row = 0; row < q8_rows; ++row)
		if (float_bits(q8_results[0][row]) !=
		    float_bits(q8_results[1][row])) {
			put("vnni-probe FAIL q8_0 forms differ in row ");
			put_number(row);
			put("\n");
			++failures;
			break;
		}

	for (Path path : both_paths)
		run_gemm(path, gemm_c[path == Path::vnni]);
	int mismatches = 0;
	u64 checksum = 0;
	for (int m = 0; m < gemm_m; ++m)
		for (int n = 0; n < gemm_n; ++n) {
			i64 want = 0;
			for (int k = 0; k < gemm_k; ++k)
				want += static_cast<i64>(gemm_a[m][k]) *
					gemm_b[k / 4][n][k % 4];
			mismatches += gemm_c[0][m][n] != want;
			mismatches += gemm_c[1][m][n] != want;
			checksum = checksum * 31 + static_cast<u32>(want);
		}
	put("vnni-probe gemm checksum=");
	put_hex(checksum);
	put(mismatches == 0 ? " ok\n" : " FAIL\n");
	failures += mismatches != 0;

	i64 dot16_want = 0;
	for (int i = 0; i < dot16_length; ++i)
		dot16_want += static_cast<i64>(dot16_x[i]) * dot16_y[i];
	bool dot16_ok = true;
	for (Path path : both_paths)
		dot16_ok = dot16_ok && run_dot16(path) == static_cast<i32>(dot16_want);
	put("vnni-probe dot16 sum=");
	put_hex(static_cast<u64>(dot16_want));
	put(dot16_ok ? " ok\n" : " FAIL\n");
	failures += !dot16_ok;

	put(failures == 0 ? "vnni-probe ok\n" : "vnni-probe FAILED\n");
	return failures;
}

/* ---- timing --------------------------------------------------------- */

u64 parse_number(const char *text)
{
	u64 value = 0;
	for (; *text >= '0' && *text <= '9'; ++text)
		value = value * 10 + static_cast<u64>(*text - '0');
	return value;
}

volatile float float_sink;
volatile i32 integer_sink;

void row(const char *kernel, const char *path, u64 batch, u64 ns,
	 u64 calls)
{
	put("bench ");
	put(kernel);
	put(" ");
	put(path);
	put(" batch=");
	put_number(batch);
	put(" ns_per_call=");
	put_number(ns / calls);
	put("\n");
}

int bench(const char *order, u64 rounds, u64 reps)
{
	const Features features = detect();
	if (!features.avx2) {
		put("vnni-probe FAIL the probe needs AVX2\n");
		return 1;
	}
	fill();
	/* Both forms run on both engines whatever CPUID says. */
	/* Volatile, so that the two timings of the control are two runs. */
	volatile u64 control_iterations = 2000000;
	u64 batch = 0;
	/* Warm both forms once, so translation is not in a batch. */
	for (Path path : both_paths) {
		float_sink = run_q8(path, q8_results[0]);
		run_gemm(path, gemm_c[2]);
		integer_sink = gemm_c[2][0][0];
	}
	float_sink = control_kernel(1000);
	u64 start = now_ns();
	float_sink = control_kernel(static_cast<int>(control_iterations));
	row("control", "avx2", batch++, now_ns() - start, 1);
	for (u64 round = 0; round < rounds; ++round)
		for (const char *letter = order; *letter != '\0'; ++letter) {
			const Path path = *letter == 'V' ? Path::vnni : Path::avx2;
			start = now_ns();
			for (u64 rep = 0; rep < reps; ++rep)
				float_sink = run_q8(path, q8_results[0]);
			row("q8_0", path_name(path), batch, now_ns() - start,
			    reps);
			start = now_ns();
			for (u64 rep = 0; rep < reps; ++rep) {
				run_gemm(path, gemm_c[2]);
				integer_sink = gemm_c[2][0][0];
			}
			row("gemm", path_name(path), batch, now_ns() - start,
			    reps);
			start = now_ns();
			for (u64 rep = 0; rep < reps * 64; ++rep)
				integer_sink = run_dot16(path);
			row("dot16", path_name(path), batch, now_ns() - start,
			    reps * 64);
			++batch;
		}
	start = now_ns();
	float_sink = control_kernel(static_cast<int>(control_iterations));
	row("control", "avx2", batch++, now_ns() - start, 1);
	return 0;
}

int probe_main(long *stack)
{
	const long argc = stack[0];
	const char **argv = reinterpret_cast<const char **>(stack + 1);
	const char *mode = argc > 1 ? argv[1] : "check";
	if (equal(mode, "check"))
		return check(argc > 2 ? argv[2] : nullptr);
	if (equal(mode, "bench") && argc == 5)
		return bench(argv[2], parse_number(argv[3]),
			     parse_number(argv[4]));
	put("usage: vnni-probe check [vnni|avx2] | bench ORDER ROUNDS REPS\n");
	return 2;
}

} // namespace

extern "C" {

/* The compiler may turn a loop into these; a freestanding program has no
 * libc to provide them. */
void *memset(void *destination, int value, unsigned long count)
{
	void *result = destination;
	__asm__ volatile("rep stosb"
			 : "+D"(destination), "+c"(count)
			 : "a"(value)
			 : "memory");
	return result;
}

void *memcpy(void *destination, const void *source, unsigned long count)
{
	void *result = destination;
	__asm__ volatile("rep movsb"
			 : "+D"(destination), "+S"(source), "+c"(count)
			 :
			 : "memory");
	return result;
}

int probe_entry(long *stack)
{
	return probe_main(stack);
}
}

__asm__(".text\n"
	".global _start\n"
	".type _start,@function\n"
	"_start:\n"
	"	xor %ebp, %ebp\n"
	"	mov %rsp, %rdi\n"
	"	and $-16, %rsp\n"
	"	call probe_entry\n"
	"	mov %eax, %edi\n"
	"	mov $231, %eax\n"
	"	syscall\n"
	"	hlt\n"
	".size _start,.-_start\n");
