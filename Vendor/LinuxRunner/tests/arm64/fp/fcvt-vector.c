#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct vec128 {
    uint64_t lo;
    uint64_t hi;
};

void fcvtn_h(const struct vec128 *src, struct vec128 *dst);
void fcvtn2_h(const struct vec128 *src, struct vec128 *dst);
void fcvtn2_h_alias(struct vec128 *value);
void fcvtn_s(const struct vec128 *src, struct vec128 *dst);
void fcvtn2_s(const struct vec128 *src, struct vec128 *dst);
void fcvtn2_s_alias(struct vec128 *value);
void fcvtxn_s(const struct vec128 *src, struct vec128 *dst);
void fcvtxn2_s(const struct vec128 *src, struct vec128 *dst);
void fcvtxn2_s_alias(struct vec128 *value);
void fcvtl_s(const struct vec128 *src, struct vec128 *dst);
void fcvtl2_s(const struct vec128 *src, struct vec128 *dst);
void fcvtl_d(const struct vec128 *src, struct vec128 *dst);
void fcvtl2_d(const struct vec128 *src, struct vec128 *dst);

static inline uint64_t read_fpcr(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, fpcr" : "=r"(value));
    return value;
}

static inline void write_fpcr(uint64_t value) {
    __asm__ volatile("msr fpcr, %0" : : "r"(value));
}

static inline uint64_t read_fpsr(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, fpsr" : "=r"(value));
    return value;
}

static inline void write_fpsr(uint64_t value) {
    __asm__ volatile("msr fpsr, %0" : : "r"(value));
}

__asm__(
".text\n"
".align 2\n"
"fcvtn_h:\n"
"    ldr q0, [x0]\n"
"    .inst 0x0e216800\n" // fcvtn v0.4h, v0.4s
"    str q0, [x1]\n"
"    ret\n"
"fcvtn2_h:\n"
"    ldr q0, [x0]\n"
"    ldr q1, [x1]\n"
"    .inst 0x4e216801\n" // fcvtn2 v1.8h, v0.4s
"    str q1, [x1]\n"
"    ret\n"
"fcvtn2_h_alias:\n"
"    ldr q0, [x0]\n"
"    .inst 0x4e216800\n" // fcvtn2 v0.8h, v0.4s
"    str q0, [x0]\n"
"    ret\n"
"fcvtn_s:\n"
"    ldr q0, [x0]\n"
"    .inst 0x0e616800\n" // fcvtn v0.2s, v0.2d
"    str q0, [x1]\n"
"    ret\n"
"fcvtn2_s:\n"
"    ldr q0, [x0]\n"
"    ldr q1, [x1]\n"
"    .inst 0x4e616801\n" // fcvtn2 v1.4s, v0.2d
"    str q1, [x1]\n"
"    ret\n"
"fcvtn2_s_alias:\n"
"    ldr q0, [x0]\n"
"    .inst 0x4e616800\n" // fcvtn2 v0.4s, v0.2d
"    str q0, [x0]\n"
"    ret\n"
"fcvtxn_s:\n"
"    ldr q0, [x0]\n"
"    .inst 0x2e616800\n" // fcvtxn v0.2s, v0.2d
"    str q0, [x1]\n"
"    ret\n"
"fcvtxn2_s:\n"
"    ldr q0, [x0]\n"
"    ldr q1, [x1]\n"
"    .inst 0x6e616801\n" // fcvtxn2 v1.4s, v0.2d
"    str q1, [x1]\n"
"    ret\n"
"fcvtxn2_s_alias:\n"
"    ldr q0, [x0]\n"
"    .inst 0x6e616800\n" // fcvtxn2 v0.4s, v0.2d
"    str q0, [x0]\n"
"    ret\n"
"fcvtl_s:\n"
"    ldr q0, [x0]\n"
"    .inst 0x0e217800\n" // fcvtl v0.4s, v0.4h
"    str q0, [x1]\n"
"    ret\n"
"fcvtl2_s:\n"
"    ldr q0, [x0]\n"
"    .inst 0x4e217800\n" // fcvtl2 v0.4s, v0.8h
"    str q0, [x1]\n"
"    ret\n"
"fcvtl_d:\n"
"    ldr q0, [x0]\n"
"    .inst 0x0e617800\n" // fcvtl v0.2d, v0.2s
"    str q0, [x1]\n"
"    ret\n"
"fcvtl2_d:\n"
"    ldr q0, [x0]\n"
"    .inst 0x4e617800\n" // fcvtl2 v0.2d, v0.4s
"    str q0, [x1]\n"
"    ret\n"
);

static int failures;

static void expect(const char *name, struct vec128 got, struct vec128 want) {
    if (got.lo == want.lo && got.hi == want.hi)
        return;
    fprintf(stderr,
            "%s: got %016llx %016llx, want %016llx %016llx\n",
            name,
            (unsigned long long)got.hi, (unsigned long long)got.lo,
            (unsigned long long)want.hi, (unsigned long long)want.lo);
    failures++;
}

int main(void) {
    const struct vec128 single_source = {
        .lo = UINT64_C(0xc00000003f800000),
        .hi = UINT64_C(0x3f000000477fe000),
    };
    const struct vec128 double_halfway = {
        .lo = UINT64_C(0x3ff0000010000000),
        .hi = UINT64_C(0xbff0000010000000),
    };
    const struct vec128 half_source = {
        .lo = UINT64_C(0x38007bffc0003c00),
        .hi = UINT64_C(0xc4004200bc003400),
    };
    const struct vec128 float_source = {
        .lo = UINT64_C(0xc00000003f800000),
        .hi = UINT64_C(0xc080000040400000),
    };
    struct vec128 value;

    value = (struct vec128){UINT64_MAX, UINT64_MAX};
    fcvtn_h(&single_source, &value);
    expect("fcvtn s-to-h lower+zero", value,
           (struct vec128){UINT64_C(0x38007bffc0003c00), 0});

    value = (struct vec128){UINT64_C(0x4444333322221111), UINT64_C(0xccccbbbbaaaa9999)};
    fcvtn2_h(&single_source, &value);
    expect("fcvtn2 s-to-h preserve", value,
           (struct vec128){UINT64_C(0x4444333322221111), UINT64_C(0x38007bffc0003c00)});

    value = single_source;
    fcvtn2_h_alias(&value);
    expect("fcvtn2 s-to-h alias", value,
           (struct vec128){single_source.lo, UINT64_C(0x38007bffc0003c00)});

    uint64_t saved_fpcr = read_fpcr();
    uint64_t saved_fpsr = read_fpsr();
    write_fpcr((saved_fpcr & ~UINT64_C(0xc00000)) | UINT64_C(0x400000));
    write_fpsr(UINT64_C(0x1)); // Pre-set IOC to check cumulative FPSR handling.
    value = (struct vec128){UINT64_MAX, UINT64_MAX};
    fcvtn_s(&double_halfway, &value);
    expect("fcvtn d-to-s fpcr+zero", value,
           (struct vec128){UINT64_C(0xbf8000003f800001), 0});
    uint64_t conversion_fpsr = read_fpsr();
    if ((conversion_fpsr & UINT64_C(0x1f)) != UINT64_C(0x11)) {
        fprintf(stderr,
                "fcvtn d-to-s FPSR: got %#llx, want cumulative IOC|IXC\n",
                (unsigned long long)conversion_fpsr);
        failures++;
    }
    write_fpcr(saved_fpcr);
    write_fpsr(saved_fpsr);

    value = (struct vec128){UINT64_C(0x2222222211111111), UINT64_C(0x4444444433333333)};
    fcvtn2_s(&double_halfway, &value);
    expect("fcvtn2 d-to-s preserve", value,
           (struct vec128){UINT64_C(0x2222222211111111), UINT64_C(0xbf8000003f800000)});

    value = double_halfway;
    fcvtn2_s_alias(&value);
    expect("fcvtn2 d-to-s alias", value,
           (struct vec128){double_halfway.lo, UINT64_C(0xbf8000003f800000)});

    value = (struct vec128){UINT64_MAX, UINT64_MAX};
    fcvtxn_s(&double_halfway, &value);
    expect("fcvtxn d-to-s round-odd+zero", value,
           (struct vec128){UINT64_C(0xbf8000013f800001), 0});

    value = (struct vec128){UINT64_C(0x2222222211111111), UINT64_C(0x4444444433333333)};
    fcvtxn2_s(&double_halfway, &value);
    expect("fcvtxn2 d-to-s preserve", value,
           (struct vec128){UINT64_C(0x2222222211111111), UINT64_C(0xbf8000013f800001)});

    value = double_halfway;
    fcvtxn2_s_alias(&value);
    expect("fcvtxn2 d-to-s alias", value,
           (struct vec128){double_halfway.lo, UINT64_C(0xbf8000013f800001)});

    value = (struct vec128){0, 0};
    fcvtl_s(&half_source, &value);
    expect("fcvtl h-to-s lower", value, single_source);

    value = (struct vec128){0, 0};
    fcvtl2_s(&half_source, &value);
    expect("fcvtl2 h-to-s upper", value,
           (struct vec128){UINT64_C(0xbf8000003e800000), UINT64_C(0xc080000040400000)});

    value = (struct vec128){0, 0};
    fcvtl_d(&float_source, &value);
    expect("fcvtl s-to-d lower", value,
           (struct vec128){UINT64_C(0x3ff0000000000000), UINT64_C(0xc000000000000000)});

    value = (struct vec128){0, 0};
    fcvtl2_d(&float_source, &value);
    expect("fcvtl2 s-to-d upper", value,
           (struct vec128){UINT64_C(0x4008000000000000), UINT64_C(0xc010000000000000)});

    if (failures != 0)
        return 1;
    puts("fcvt-vector-ok");
    return 0;
}
