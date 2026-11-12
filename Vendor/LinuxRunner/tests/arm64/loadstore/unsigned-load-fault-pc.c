#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#if !defined(__aarch64__)
#error "requires AArch64 Linux guest or native Linux"
#endif

/* Keep the instruction before LDR observable. Retrying the block rather than
 * the LDR increments x5 twice; skipping it or reporting an earlier PC fails in
 * the signal handler. CBZ/CBNZ must remain immediately adjacent to the load so
 * the translator exercises its load/branch fusion as well as the plain path.
 */
#define LOAD_CASE(name, instruction, branch) \
    __asm__(".text\n.p2align 4\n.global " #name "\n" \
            ".type " #name ", %function\n" #name ":\n" \
            "mov x5, #0\nmov x6, #0x55\nmov x3, #0\n" \
            "add x5, x5, #1\n.global " #name "_pc\n" #name "_pc:\n" \
            instruction "\n" branch \
            "b 2f\n1: mov x3, #1\n2:\n" \
            "str x6, [x1]\nstr x5, [x1, #8]\nstr x3, [x1, #16]\nret\n" \
            ".size " #name ", .-" #name "\n"); \
    extern void name(const void *, uint64_t *); \
    extern const char name##_pc[]

LOAD_CASE(load8_plain, "ldrb w6, [x0, #8]", "");
LOAD_CASE(load8_cbz, "ldrb w6, [x0, #8]", "cbz w6, 1f\n");
LOAD_CASE(load8_cbnz, "ldrb w6, [x0, #8]", "cbnz w6, 1f\n");
LOAD_CASE(load16_plain, "ldrh w6, [x0, #8]", "");
LOAD_CASE(load16_cbz, "ldrh w6, [x0, #8]", "cbz w6, 1f\n");
LOAD_CASE(load16_cbnz, "ldrh w6, [x0, #8]", "cbnz w6, 1f\n");
LOAD_CASE(load32_plain, "ldr w6, [x0, #8]", "");
LOAD_CASE(load32_cbz, "ldr w6, [x0, #8]", "cbz w6, 1f\n");
LOAD_CASE(load32_cbnz, "ldr w6, [x0, #8]", "cbnz w6, 1f\n");
LOAD_CASE(load64_plain, "ldr x6, [x0, #8]", "");
LOAD_CASE(load64_cbz, "ldr x6, [x0, #8]", "cbz x6, 1f\n");
LOAD_CASE(load64_cbnz, "ldr x6, [x0, #8]", "cbnz x6, 1f\n");

static void *protected_page;
static size_t page_size, protected_length;
static uintptr_t expected_pc;
static volatile sig_atomic_t faults;
static unsigned char saved_pages[8192];

static void handler(int sig, siginfo_t *info, void *context) {
    (void)info;
    ucontext_t *uc = context;
    if (sig != SIGSEGV || uc->uc_mcontext.pc != expected_pc ||
        uc->uc_mcontext.regs[5] != 1 || uc->uc_mcontext.regs[6] != 0x55)
        _exit(40);
    if (++faults != 1)
        _exit(41);
    void *p = mmap(protected_page, protected_length, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p != protected_page)
        _exit(42);
    memcpy(p, saved_pages, protected_length);
}

int main(void) {
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != 4096)
        return 10;
    // Isolated fixed region: avoid both grow-down stack recovery and the
    // existing near-neighbor read-fault auto-mapping workaround. Removing
    // both pages below gives the handler a real SIGSEGV even for split loads.
    unsigned char *mem = mmap((void *)0x40000000, page_size * 2, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mem == MAP_FAILED)
        return 11;
    struct sigaction sa = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL))
        return 12;
    alarm(30);
    void (*loads[])(const void *, uint64_t *) = {load8_plain, load8_cbz, load8_cbnz, load16_plain, load16_cbz, load16_cbnz, load32_plain, load32_cbz, load32_cbnz, load64_plain, load64_cbz, load64_cbnz};
    const char *pcs[] = {load8_plain_pc, load8_cbz_pc, load8_cbnz_pc, load16_plain_pc, load16_cbz_pc, load16_cbnz_pc, load32_plain_pc, load32_cbz_pc, load32_cbnz_pc, load64_plain_pc, load64_cbz_pc, load64_cbnz_pc};
    const size_t offsets[] = {64, 67, 4092};
    const uint64_t values[] = {0, UINT64_C(0xfedcba9876543210)};
    unsigned cases = 0;
    for (unsigned fn = 0; fn < 12; fn++) {
        for (unsigned off = 0; off < 3; off++) {
            for (unsigned val = 0; val < 2; val++) {
              for (unsigned protection = 0; protection < 4; protection++) {
                if (protection >= 2 && off != 2) continue;
                unsigned width = 1u << (fn / 3);
                unsigned char *address = mem + (off == 2 ? 4096 - (width > 1 ? width / 2 : 1) : offsets[off]);
                if (protection >= 2 && width == 1) address = mem + page_size;
                memcpy(address, &values[val], 8);
                uint64_t value = 0; memcpy(&value, &values[val], width);
                uint64_t expected_branch = fn % 3 == 1 ? value == 0 : fn % 3 == 2 ? value != 0 : 0;
                uint64_t out[3] = {0};
                // Warm code and translations, then remove both backing pages.
                // The second invocation must fault and retry precisely LDR.
                // The handler restores both pages. For split loads this tests
                // first-page fault recovery; modes 2/3 keep the first page
                // readable and fault only the second half of a split load.
                // Repeat with mapped PROT_NONE pages to verify permissions.
                loads[fn](address - 8, out);
                if (out[0] != value || out[1] != 1 || out[2] != expected_branch)
                    return 20;
                protected_page = protection < 2 ? mem : mem + page_size;
                protected_length = protection < 2 ? page_size * 2 : page_size;
                expected_pc = (uintptr_t)pcs[fn];
                faults = 0;
                memcpy(saved_pages, protected_page, protected_length);
                if (protection % 2 == 0) {
                    if (munmap(protected_page, protected_length)) return 21;
                } else if (mprotect(protected_page, protected_length, PROT_NONE)) return 21;
                loads[fn](address - 8, out);
                if (faults != 1 || out[0] != value || out[1] != 1 || out[2] != expected_branch) {
                    fprintf(stderr, "fn=%u off=%zu val=%u faults=%d out=%llx,%llu,%llu expected=%llx,1,%llu\n",
                            fn, offsets[off], val, faults, (unsigned long long)out[0],
                            (unsigned long long)out[1], (unsigned long long)out[2],
                            (unsigned long long)values[val], (unsigned long long)expected_branch);
                    return 22;
                }
                cases++;
              }
            }
        }
    }
    if (munmap(mem, page_size * 2))
        return 23;
    printf("unsigned-load-fault-pc-ok cases=%u\n", cases);
    return 0;
}
