// Precise memory-fault PCs: for each load/store form, a SIGSEGV on a
// PROT_NONE page must report the faulting instruction's own PC, and after the
// handler makes the page accessible the retry must run that instruction only
// (an earlier side effect in the same block runs exactly once; a faulting
// pre/post-index access writes its base back exactly once). The interpreter
// recovers these PCs from a per-block table (ISH_LAZY_FAULT_PC) instead of a
// run-time store before every access.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

static uint8_t *page;
static volatile uint64_t fault_pc, fault_addr;
static volatile int faults;
static int failures;

static void on_segv(int sig, siginfo_t *info, void *context) {
    (void) sig;
    ucontext_t *uc = context;
    faults++;
    fault_pc = uc->uc_mcontext.pc;
    fault_addr = (uint64_t) info->si_addr;
    mprotect(page, 4096, PROT_READ | PROT_WRITE);
}

static void arm(void) { faults = 0; fault_pc = 0; memset(page, 0x5a, 4096); mprotect(page, 4096, PROT_NONE); }

static void report(const char *name, uint64_t expect_pc, long side, long expect_side, long base_moves, long expect_moves) {
    int ok = faults == 1 && fault_pc == expect_pc && side == expect_side && base_moves == expect_moves;
    printf("%s %s faults=%d pc=%s side=%ld base=%ld\n", ok ? "ok  " : "FAIL", name, faults,
           fault_pc == expect_pc ? "exact" : "WRONG", side, base_moves);
    if (!ok) failures++;
}

// x9 counts the side effect before the access; `at` receives the access PC.
// `pre` runs between the side effect and the faulting instruction (labelled 1).
#define CASE(name, pre, insn, moves_expr, ...) do { \
    arm(); uint64_t at; long side; uint64_t base = (uint64_t) page + 64, base0 = base; \
    __asm__ volatile(".arch_extension lse\n\tmov x9, #0\n\tadd x9, x9, #1\n\t" pre "\n1:\t" insn "\n\tadd x9, x9, #1\n\tadr %[at], 1b\n\tmov %[side], x9" \
                     : [at] "=&r"(at), [side] "=&r"(side), [b] "+&r"(base) : __VA_ARGS__ \
                     : "x9", "x10", "x11", "v0", "memory"); \
    report(name, at, side, 2, (long) (base - base0), moves_expr); } while (0)

int main(void) {
    page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct sigaction sa = {0}; sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL);
    uint64_t off = 8, one = 1, cmpv = 0x5a5a5a5a5a5a5a5aULL;

    CASE("ldr-imm", "", "ldr x10, [%[b], #16]", 0, "r"(off));
    CASE("ldr-reg", "", "ldr x10, [%[b], %[o]]", 0, [o] "r"(off));
    CASE("ldr-pre", "", "ldr x10, [%[b], #8]!", 8, "r"(off));
    CASE("ldr-post", "", "ldr x10, [%[b]], #8", 8, "r"(off));
    CASE("ldur", "", "ldur x10, [%[b], #-3]", 0, "r"(off));
    CASE("ldrb", "", "ldrb w10, [%[b], #5]", 0, "r"(off));
    CASE("ldp", "", "ldp x10, x11, [%[b], #16]", 0, "r"(off));
    CASE("ldp-post", "", "ldp x10, x11, [%[b]], #16", 16, "r"(off));
    CASE("str-imm", "", "str x9, [%[b], #24]", 0, "r"(off));
    CASE("str-reg", "", "str x9, [%[b], %[o]]", 0, [o] "r"(off));
    CASE("stp-pre", "", "stp x9, x9, [%[b], #-16]!", -16, "r"(off));
    CASE("add-ldr", "add x10, %[b], #8", "ldr x11, [x10, #8]", 0, "r"(off));
    CASE("ldadd", "", "ldadd %[one], x10, [%[b]]", 0, [one] "r"(one));
    CASE("cas", "mov x10, %[c]", "cas x10, %[one], [%[b]]", 0, [one] "r"(one), [c] "r"(cmpv));
    CASE("ldxr-stxr", "", "ldxr x10, [%[b]]\n\tstxr w11, x10, [%[b]]", 0, "r"(off));
    CASE("ld1", "", "ld1 {v0.16b}, [%[b]]", 0, "r"(off));
    CASE("st1-post", "", "st1 {v0.16b}, [%[b]], #16", 16, "r"(off));

    // DC ZVA zeroes a 64-byte block: base aligned to 64.
    arm(); { uint64_t at; long side; uint64_t base = (uint64_t) page;
        __asm__ volatile("mov x9, #0\n\tadd x9, x9, #1\n1:\tdc zva, %[b]\n\tadd x9, x9, #1\n\tadr %[at], 1b\n\tmov %[side], x9"
                         : [at] "=&r"(at), [side] "=&r"(side) : [b] "r"(base) : "x9", "memory");
        report("dc-zva", at, side, 2, 0, 0);
        if (page[0] != 0 || page[63] != 0 || page[64] != 0x5a) { printf("FAIL dc-zva contents\n"); failures++; } }

    printf("PRECISE_FAULT_PC_CONTRACT %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
