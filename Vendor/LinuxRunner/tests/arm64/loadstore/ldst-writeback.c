#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

/* Single-register LDUR/STUR and pre/post-indexed LDR/STR (all sizes, zero and
 * sign extension) and pre/post-indexed STP/LDP, with an ordinary base and with
 * SP as the base, at negative offsets and across a page boundary; then a fault
 * in each writeback form, which must retry the instruction once without
 * writing back the base register twice (or before the access succeeded).
 * Expected values are architectural, so the result does not depend on the
 * interpreter's gadget selection.
 */
static unsigned failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static unsigned char *pages;   // two pages at a fixed address; page 1 starts at pages + 4096

static uint64_t le(const unsigned char *p, unsigned n) { uint64_t v = 0; memcpy(&v, p, n); return v; }
static int64_t sx(uint64_t v, unsigned bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

static void unscaled_loads(unsigned char *p) {
    uint64_t r; unsigned char *b = p + 16;   // base; loads at b-9 (negative, unaligned)
    __asm__ volatile("ldurb w0, [%1, #-9]\nmov %0, x0" : "=r"(r) : "r"(b) : "x0", "memory"); CHECK(r == le(p + 7, 1), "ldurb");
    __asm__ volatile("ldurh w0, [%1, #-9]\nmov %0, x0" : "=r"(r) : "r"(b) : "x0", "memory"); CHECK(r == le(p + 7, 2), "ldurh");
    __asm__ volatile("ldur w0, [%1, #-9]\nmov %0, x0" : "=r"(r) : "r"(b) : "x0", "memory"); CHECK(r == le(p + 7, 4), "ldur w");
    __asm__ volatile("ldur x0, [%1, #-9]\nmov %0, x0" : "=r"(r) : "r"(b) : "x0", "memory"); CHECK(r == le(p + 7, 8), "ldur x");
    __asm__ volatile("ldursb w0, [%1, #-9]\nmov %0, x0" : "=r"(r) : "r"(b) : "x0", "memory"); CHECK(r == (uint32_t)sx(le(p + 7, 1), 8), "ldursb w");
    __asm__ volatile("ldursh w0, [%1, #-9]\nmov %0, x0" : "=r"(r) : "r"(b) : "x0", "memory"); CHECK(r == (uint32_t)sx(le(p + 7, 2), 16), "ldursh w");
    __asm__ volatile("ldursb x0, [%1, #-9]\nmov %0, x0" : "=r"(r) : "r"(b) : "x0", "memory"); CHECK(r == (uint64_t)sx(le(p + 7, 1), 8), "ldursb x");
    __asm__ volatile("ldursh x0, [%1, #-9]\nmov %0, x0" : "=r"(r) : "r"(b) : "x0", "memory"); CHECK(r == (uint64_t)sx(le(p + 7, 2), 16), "ldursh x");
    __asm__ volatile("ldursw x0, [%1, #-9]\nmov %0, x0" : "=r"(r) : "r"(b) : "x0", "memory"); CHECK(r == (uint64_t)sx(le(p + 7, 4), 32), "ldursw");
}

static void indexed(unsigned char *p, const char *where) {
    uint64_t v, base;
    // pre-index load: base += -8 first, then load
    base = (uint64_t)(p + 24);
    __asm__ volatile("ldr %0, [%1, #-8]!" : "=&r"(v), "+r"(base) : : "memory");
    CHECK(v == le(p + 16, 8) && base == (uint64_t)(p + 16), "%s ldr pre", where);
    base = (uint64_t)(p + 24);
    __asm__ volatile("ldrsh %w0, [%1, #-3]!" : "=&r"(v), "+r"(base) : : "memory");
    CHECK((uint32_t)v == (uint32_t)sx(le(p + 21, 2), 16) && base == (uint64_t)(p + 21), "%s ldrsh w pre", where);
    // post-index load: load at base, then base += 16
    base = (uint64_t)(p + 8);
    __asm__ volatile("ldr %w0, [%1], #16" : "=&r"(v), "+r"(base) : : "memory");
    CHECK(v == le(p + 8, 4) && base == (uint64_t)(p + 24), "%s ldr w post", where);
    base = (uint64_t)(p + 40);
    __asm__ volatile("ldrsb %0, [%1], #-33" : "=&r"(v), "+r"(base) : : "memory");
    CHECK(v == (uint64_t)sx(le(p + 40, 1), 8) && base == (uint64_t)(p + 7), "%s ldrsb x post", where);
    base = (uint64_t)(p + 40);
    __asm__ volatile("ldrsw %0, [%1, #255]!" : "=&r"(v), "+r"(base) : : "memory");
    CHECK(v == (uint64_t)sx(le(p + 295, 4), 32) && base == (uint64_t)(p + 295), "%s ldrsw pre +255", where);
    // stores
    unsigned char before[512]; memcpy(before, p, 512);
    uint64_t val = UINT64_C(0x8877665544332211);
    base = (uint64_t)(p + 64);
    __asm__ volatile("str %1, [%0, #-256]!" : "+r"(base) : "r"(val) : "memory");
    CHECK(base == (uint64_t)(p + 64 - 256) && le(p + 64 - 256, 8) == val, "%s str pre -256", where);
    base = (uint64_t)(p + 100);
    __asm__ volatile("strh %w1, [%0], #-5" : "+r"(base) : "r"(val) : "memory");
    CHECK(base == (uint64_t)(p + 95) && le(p + 100, 2) == 0x2211, "%s strh post", where);
    base = (uint64_t)(p + 120);
    __asm__ volatile("strb %w1, [%0, #3]!" : "+r"(base) : "r"(val) : "memory");
    CHECK(base == (uint64_t)(p + 123) && p[123] == 0x11 && p[122] == before[122] && p[124] == before[124], "%s strb pre", where);
    base = (uint64_t)(p + 130);
    __asm__ volatile("stur %w1, [%0, #-1]" : : "r"(base), "r"(val) : "memory");
    CHECK(le(p + 129, 4) == 0x44332211 && p[133] == before[133], "%s stur w", where);
    // pairs
    uint64_t a = UINT64_C(0x1111222233334444), c = UINT64_C(0x5555666677778888);
    base = (uint64_t)(p + 208);
    __asm__ volatile("stp %1, %2, [%0, #-32]!" : "+r"(base) : "r"(a), "r"(c) : "memory");
    CHECK(base == (uint64_t)(p + 176) && le(p + 176, 8) == a && le(p + 184, 8) == c, "%s stp x pre", where);
    base = (uint64_t)(p + 300);
    __asm__ volatile("stp %w1, %w2, [%0], #-12" : "+r"(base) : "r"(a), "r"(c) : "memory");
    CHECK(base == (uint64_t)(p + 288) && le(p + 300, 4) == 0x33334444 && le(p + 304, 4) == 0x77778888, "%s stp w post", where);
    uint64_t l1, l2;
    base = (uint64_t)(p + 176);
    __asm__ volatile("ldp %0, %1, [%2], #32" : "=&r"(l1), "=&r"(l2), "+r"(base) : : "memory");
    CHECK(l1 == a && l2 == c && base == (uint64_t)(p + 208), "%s ldp x post", where);
    base = (uint64_t)(p + 312);
    __asm__ volatile("ldp %w0, %w1, [%2, #-12]!" : "=&r"(l1), "=&r"(l2), "+r"(base) : : "memory");
    CHECK(l1 == 0x33334444 && l2 == 0x77778888 && base == (uint64_t)(p + 300), "%s ldp w pre", where);
}

// SP as the base register: a 64-byte frame below the current stack.
static void sp_base(void) {
    uint64_t out[8];
    __asm__ volatile(
        "mov x9, sp\n"
        "mov x10, #0x1234\n movk x10, #0xabcd, lsl #48\n"
        "mov x11, #0x5678\n"
        "sub sp, sp, #64\n"
        "stp x10, x11, [sp, #-16]!\n"       // sp -= 16
        "mov x12, sp\n sub x12, x9, x12\n str x12, [%0]\n"      // expect 80
        "ldr x13, [sp], #16\n str x13, [%0, #8]\n"               // x10, sp += 16
        "ldur x13, [sp, #-8]\n str x13, [%0, #16]\n"             // x11
        "str x10, [sp, #-16]!\n ldr x13, [sp]\n str x13, [%0, #24]\n"
        "ldr w13, [sp, #4]\n str x13, [%0, #32]\n"
        "stp w10, w11, [sp], #16\n ldur x13, [sp, #-16]\n str x13, [%0, #40]\n"
        "mov x12, sp\n sub x12, x9, x12\n str x12, [%0, #48]\n"  // expect 64
        "mov sp, x9\n"
        : : "r"(out) : "x9", "x10", "x11", "x12", "x13", "memory");
    uint64_t x10 = UINT64_C(0xabcd000000001234), x11 = 0x5678;
    CHECK(out[0] == 80, "sp stp pre writeback %llu", (unsigned long long)out[0]);
    CHECK(out[1] == x10, "sp ldr post");
    CHECK(out[2] == x11, "sp ldur");
    CHECK(out[3] == x10, "sp str pre");
    CHECK(out[4] == (x10 >> 32), "sp ldr w imm");
    CHECK(out[5] == ((x11 << 32) | (x10 & 0xffffffff)), "sp stp w post");
    CHECK(out[6] == 64, "sp final %llu", (unsigned long long)out[6]);
}

// Faults: each instruction runs once after the handler maps the page.
#define FAULTER(name, instruction) \
 __asm__(".text\n.p2align 4\n.global " #name "\n.type " #name ", %function\n" \
 #name ":\nmov x5, #0\nmov x6, x1\nadd x5, x5, #1\n.global " #name "_pc\n" #name "_pc:\n" instruction "\n" \
 "str x0, [x2]\nstr x5, [x2, #8]\nstr x6, [x2, #16]\nret\n.size " #name ", .-" #name "\n"); \
 extern void name(void *base, uint64_t value, uint64_t *out); extern const char name##_pc[]
FAULTER(f_str_pre, "str x6, [x0, #16]!");
FAULTER(f_strh_post, "strh w6, [x0], #-6");
FAULTER(f_ldr_pre, "ldr x6, [x0, #-8]!");
FAULTER(f_ldrsw_post, "ldrsw x6, [x0], #12");
FAULTER(f_ldur, "ldur x6, [x0, #-3]");
FAULTER(f_stp_pre, "stp x6, x1, [x0, #-16]!");
FAULTER(f_stp_post, "stp w6, w1, [x0], #8");

static uintptr_t expected_pc;
static volatile sig_atomic_t faults;
static unsigned char *fault_page;
static void handler(int sig, siginfo_t *info, void *context) {
    (void)info; ucontext_t *uc = context;
    if (sig != SIGSEGV || uc->uc_mcontext.pc != expected_pc || uc->uc_mcontext.regs[5] != 1) _exit(40);
    if (++faults != 1) _exit(41);
    if (mprotect(fault_page, 4096, PROT_READ | PROT_WRITE)) _exit(42);
}

static void fault_case(void (*fn)(void *, uint64_t, uint64_t *), const char *pc, const char *name,
                       unsigned offset, int64_t base_delta, uint64_t expect_x6) {
    uint64_t out[3] = {0};
    expected_pc = (uintptr_t)pc; faults = 0;
    if (mprotect(fault_page, 4096, PROT_NONE)) { failures++; return; }
    fn(pages + offset, UINT64_C(0x0102030405060708), out);
    CHECK(faults == 1, "%s faults=%d", name, (int)faults);
    CHECK(out[0] == (uint64_t)(pages + offset + base_delta), "%s base %#llx", name, (unsigned long long)(out[0] - (uint64_t)pages));
    CHECK(out[1] == 1, "%s retried earlier instruction", name);
    CHECK(out[2] == expect_x6, "%s value %#llx", name, (unsigned long long)out[2]);
}

int main(void) {
    if (sysconf(_SC_PAGESIZE) != 4096) return 10;
    pages = mmap((void *)0x50000000, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (pages == MAP_FAILED) return 11;
    for (unsigned i = 0; i < 8192; i++) pages[i] = (unsigned char)(i * 37 + 0x81);
    unscaled_loads(pages);
    unscaled_loads(pages + 4096 - 12);     // b-9 spans the page boundary for 2/4/8-byte loads
    indexed(pages + 512, "mid");
    indexed(pages + 4096 - 300, "cross");  // several accesses straddle page 0/1
    sp_base();

    struct sigaction sa = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO}; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL)) return 12;
    alarm(30);
    fault_page = pages + 4096;
    for (unsigned i = 0; i < 4096; i++) pages[4096 + i] = (unsigned char)i;
    uint64_t v = UINT64_C(0x0102030405060708);
    fault_case(f_str_pre, f_str_pre_pc, "str pre", 4096 - 16, 16, v);
    fault_case(f_strh_post, f_strh_post_pc, "strh post", 4096 + 40, -6, v);
    fault_case(f_ldr_pre, f_ldr_pre_pc, "ldr pre", 4096 + 16, -8, le(pages + 4096 + 8, 8));
    fault_case(f_ldrsw_post, f_ldrsw_post_pc, "ldrsw post", 4096 + 64, 12, (uint64_t)sx(le(pages + 4096 + 64, 4), 32));
    fault_case(f_ldur, f_ldur_pc, "ldur", 4096 + 2, 0, le(pages + 4096 - 1, 8));   // page-split: first byte on page 0
    fault_case(f_stp_pre, f_stp_pre_pc, "stp pre", 4096 + 32, -16, v);
    fault_case(f_stp_post, f_stp_post_pc, "stp post", 4096 + 8, 8, v);
    CHECK(le(pages + 4096 + 16, 8) == v && le(pages + 4096 + 24, 8) == v, "stp pre data");
    CHECK(le(pages + 4096 + 8, 4) == 0x05060708 && le(pages + 4096 + 12, 4) == 0x05060708, "stp post data");

    printf("%s checks=%u failures=%u\n", failures ? "LDST_WRITEBACK_FAIL" : "LDST_WRITEBACK_OK", checks, failures);
    return failures != 0;
}
