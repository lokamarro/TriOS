// SQSHL/UQSHL/SQSHLU (vector, immediate): lanes and cumulative FPSR.QC versus a scalar
// reference. The same source runs natively (validating the reference) and in
// the guest (validating the interpreter). Build 880 dav1d-worker trapped on
// 0x4f117610 = SQSHL v16.8H, v16.8H, #1.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned failures, cases;
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static int64_t sat_s(__int128 v, int bits) {
    __int128 hi = ((__int128) 1 << (bits - 1)) - 1, lo = -hi - 1;
    return v > hi ? (int64_t) hi : v < lo ? (int64_t) lo : (int64_t) v;
}
static uint64_t sat_u(unsigned __int128 v, int bits) {
    unsigned __int128 hi = bits == 64 ? (unsigned __int128) UINT64_MAX : (((unsigned __int128) 1 << bits) - 1);
    return v > hi ? (uint64_t) hi : (uint64_t) v;
}

// Reference for one 128-bit operand: bits = element width, lanes active = q ? all : half.
static void reference(const uint8_t in[16], uint8_t out[16], int bits, int shift, int q, int is_unsigned, int *qc) {
    int lanes = 128 / bits, active = q ? lanes : lanes / 2, bytes = bits / 8;
    memset(out, 0, 16); *qc = 0;
    for (int i = 0; i < active; i++) {
        uint64_t raw = 0; memcpy(&raw, in + i * bytes, bytes);
        uint64_t result;
        if (is_unsigned == 2) { // SQSHLU: signed input, unsigned result
            int64_t sv = bits == 64 ? (int64_t) raw : (int64_t) (raw << (64 - bits)) >> (64 - bits);
            __int128 v = (__int128) sv * ((__int128) 1 << shift);
            unsigned __int128 hi = bits == 64 ? (unsigned __int128) UINT64_MAX : (((unsigned __int128) 1 << bits) - 1);
            result = v < 0 ? 0 : (unsigned __int128) v > hi ? (uint64_t) hi : (uint64_t) v;
            if (v < 0 || (unsigned __int128) v > hi) *qc = 1;
        } else if (is_unsigned) {
            unsigned __int128 v = (unsigned __int128) raw << shift;
            result = sat_u(v, bits); if (result != (uint64_t) v) *qc = 1;
        } else {
            int64_t s = bits == 64 ? (int64_t) raw : (int64_t) (raw << (64 - bits)) >> (64 - bits);
            __int128 v = (__int128) s * ((__int128) 1 << shift);
            int64_t r = sat_s(v, bits); if ((__int128) r != v) *qc = 1;
            result = (uint64_t) r;
        }
        memcpy(out + i * bytes, &result, bytes);
    }
}

static inline uint64_t read_fpsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
static inline void clear_fpsr(void) { __asm__ volatile("msr fpsr, xzr"); }

#define RUN(OP, ARR, SHIFT, BITS, Q, UNS)                                                   \
    do {                                                                                    \
        for (int t = 0; t < 64; t++) {                                                      \
            uint8_t in[16], got[16], want[16]; uint64_t a = next(), b = next();             \
            if (t & 1) { a |= 0x7f7f7f7f7f7f7f7full; b ^= 0x8080808080808080ull; }        \
            memcpy(in, &a, 8); memcpy(in + 8, &b, 8); memset(got, 0xa5, 16);              \
            clear_fpsr();                                                                   \
            __asm__ volatile("ldr q16, [%1]\n\t" OP " v16." ARR ", v16." ARR ", #" #SHIFT "\n\t" \
                             "str q16, [%0]" :: "r"(got), "r"(in) : "v16", "memory");      \
            int qc_got = (read_fpsr() >> 27) & 1, qc_want;                                  \
            reference(in, want, BITS, SHIFT, Q, UNS, &qc_want); cases++;                    \
            if (memcmp(got, want, 16) || qc_got != qc_want) {                               \
                if (failures++ < 8) printf("FAIL %s.%s #%d case=%d qc=%d/%d\n", OP, ARR, SHIFT, t, qc_got, qc_want); \
            }                                                                               \
        }                                                                                   \
    } while (0)

#define BOTH(OP, UNS)                                                                       \
    RUN(OP, "16b", 0, 8, 1, UNS); RUN(OP, "16b", 1, 8, 1, UNS); RUN(OP, "16b", 7, 8, 1, UNS); \
    RUN(OP, "8b", 3, 8, 0, UNS);                                                            \
    RUN(OP, "8h", 0, 16, 1, UNS); RUN(OP, "8h", 1, 16, 1, UNS); RUN(OP, "8h", 15, 16, 1, UNS); \
    RUN(OP, "4h", 5, 16, 0, UNS);                                                           \
    RUN(OP, "4s", 0, 32, 1, UNS); RUN(OP, "4s", 1, 32, 1, UNS); RUN(OP, "4s", 31, 32, 1, UNS); \
    RUN(OP, "2s", 13, 32, 0, UNS);                                                          \
    RUN(OP, "2d", 0, 64, 1, UNS); RUN(OP, "2d", 1, 64, 1, UNS); RUN(OP, "2d", 33, 64, 1, UNS); \
    RUN(OP, "2d", 63, 64, 1, UNS)

int main(void) {
    BOTH("sqshl", 0);
    BOTH("uqshl", 1);
    BOTH("sqshlu", 2);
    printf("regress_saturating_shift_imm %s cases=%u failures=%u\n", failures ? "FAIL" : "PASS", cases, failures);
    return failures ? 1 : 0;
}
