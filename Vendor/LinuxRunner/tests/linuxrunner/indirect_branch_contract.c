// Indirect branch contract: BR/BLR/RET semantics the interpreter must keep
// when indirect targets chain through its block cache (ISH_INDIRECT_BRANCH_CACHE).
// Covers function-pointer and virtual-style dispatch, deep recursion through
// pointers, computed goto (BR), BLR x30, a callee that returns through a
// register other than x30, rewritten code called through the same pointer,
// a signal delivered while a thread spins in an indirect-call loop, and
// another thread swapping the pointer target.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef long (*op_fn)(long, long);
static long op_add(long a, long b) { return a + b; }
static long op_sub(long a, long b) { return a - b; }
static long op_mul(long a, long b) { return a * b; }
static long op_xor(long a, long b) { return a ^ b; }
static op_fn volatile ops[4] = {op_add, op_sub, op_mul, op_xor};

struct shape { const struct shape_ops *ops; long v; };
struct shape_ops { long (*area)(const struct shape *); long (*scale)(struct shape *, long); };
static long sq_area(const struct shape *s) { return s->v * s->v; }
static long sq_scale(struct shape *s, long k) { s->v *= k; return s->v; }
static long ln_area(const struct shape *s) { return s->v; }
static long ln_scale(struct shape *s, long k) { s->v += k; return s->v; }
static const struct shape_ops square_ops = {sq_area, sq_scale}, line_ops = {ln_area, ln_scale};

static long (*volatile recurse_ptr)(long);
static long recurse(long n) { return n == 0 ? 0 : 1 + recurse_ptr(n - 1); }

static long computed_goto(long n) {
    static void *const labels[] = {&&l0, &&l1, &&l2, &&l3};
    long acc = 0;
    for (long i = 0; i < n; i++) {
        goto *labels[i & 3];
    l0: acc += 1; continue;
    l1: acc += 10; continue;
    l2: acc += 100; continue;
    l3: acc += 1000; continue;
    }
    return acc;
}

static long blr_x30_target(void) { return 0x5a5a; }
static long call_through_x30(long (*fn)(void)) {
    long result;
    // BLR x30: the branch target is read before x30 receives the return address.
    __asm__ volatile("mov x30, %1\n\tblr x30\n\tmov %0, x0" : "=r"(result) : "r"(fn)
                     : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                       "x12", "x13", "x14", "x15", "x16", "x17", "x30", "memory", "cc");
    return result;
}

// RET x16 instead of x30: caller passes the return address in x16.
void ret_via_x16(void);
__asm__(".text\n.p2align 2\n.type ret_via_x16, %function\nret_via_x16:\n\tmov x0, #77\n\tret x16\n.size ret_via_x16, .-ret_via_x16\n");
static long call_ret_via_x16(void) {
    long result;
    __asm__ volatile("adr x16, 1f\n\tbr %1\n1:\tmov %0, x0"
                     : "=r"(result) : "r"(ret_via_x16) : "x0", "x16", "memory", "cc");
    return result;
}

static void write_return_constant(uint32_t *code, unsigned value) {
    code[0] = 0x52800000u | ((value & 0xffff) << 5);   // mov w0, #value
    code[1] = 0xd65f03c0u;                             // ret
    __builtin___clear_cache((char *) code, (char *) (code + 2));
}

static void smc_through_pointer(void) {
    uint32_t *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(code != MAP_FAILED, "mmap rwx");
    if (code == MAP_FAILED) return;
    int (*volatile fn)(void) = (int (*)(void)) code;
    int mismatches = 0;
    for (unsigned i = 0; i < 2000; i++) {
        write_return_constant(code, i);
        char stack_write[64]; memset(stack_write, (int) i, sizeof stack_write);   // later store to another page
        __asm__ volatile("" :: "r"(stack_write) : "memory");
        if (fn() != (int) i) mismatches++;
    }
    CHECK(mismatches == 0, "rewritten code returned stale results %d times", mismatches);
    munmap(code, 4096);
}

static volatile sig_atomic_t alarm_hits;
static void on_alarm(int sig) { (void) sig; alarm_hits++; }
static void signal_during_indirect_loop(void) {
    struct sigaction sa = {0}; sa.sa_handler = on_alarm; sigaction(SIGALRM, &sa, NULL);
    alarm_hits = 0;
    struct itimerspec it = {{0, 20 * 1000000}, {0, 20 * 1000000}};
    timer_t t; struct sigevent ev = {0}; ev.sigev_notify = SIGEV_SIGNAL; ev.sigev_signo = SIGALRM;
    CHECK(timer_create(CLOCK_MONOTONIC, &ev, &t) == 0, "timer_create");
    timer_settime(t, 0, &it, NULL);
    long acc = 0;
    while (alarm_hits < 5) acc = ops[acc & 3](acc, 3) & 0xffff;   // only indirect calls, no syscalls
    timer_delete(t);
    CHECK(alarm_hits >= 5, "signals not delivered during indirect-call loop");
}

static atomic_int swap_stop;
static int ret1(void) { return 1; }
static int ret2(void) { return 2; }
static int (*_Atomic shared_fn)(void) = ret1;
static void *swapper(void *arg) {
    (void) arg;
    while (!atomic_load(&swap_stop)) { atomic_store(&shared_fn, ret2); atomic_store(&shared_fn, ret1); }
    return NULL;
}
static void cross_thread_target_swap(void) {
    pthread_t th; pthread_create(&th, NULL, swapper, NULL);
    long ones = 0, twos = 0, other = 0;
    for (int i = 0; i < 200000; i++) {
        int r = atomic_load(&shared_fn)();
        if (r == 1) ones++; else if (r == 2) twos++; else other++;
    }
    atomic_store(&swap_stop, 1); pthread_join(th, NULL);
    CHECK(other == 0 && ones + twos == 200000, "swapped target produced %ld unexpected results", other);
}

int main(void) {
    long acc = 7;
    for (long i = 0; i < 400000; i++) acc = ops[i & 3](acc, i) & 0xffffffff;
    long expect = 7;
    for (long i = 0; i < 400000; i++) {
        switch (i & 3) { case 0: expect += i; break; case 1: expect -= i; break; case 2: expect *= i; break; default: expect ^= i; }
        expect &= 0xffffffff;
    }
    CHECK(acc == expect, "function-pointer dispatch %ld != %ld", acc, expect);

    struct shape shapes[64];
    for (int i = 0; i < 64; i++) { shapes[i].ops = (i & 1) ? &square_ops : &line_ops; shapes[i].v = i % 7 + 1; }
    long total = 0;
    for (int round = 0; round < 2000; round++)
        for (int i = 0; i < 64; i++) { total += shapes[i].ops->area(&shapes[i]); shapes[i].ops->scale(&shapes[i], (i & 1) ? 1 : -0); }
    long expect_total = 0;
    for (int i = 0; i < 64; i++) expect_total += 2000L * ((i & 1) ? (long) (i % 7 + 1) * (i % 7 + 1) : (i % 7 + 1));
    CHECK(total == expect_total, "virtual-style dispatch %ld != %ld", total, expect_total);

    recurse_ptr = recurse;
    CHECK(recurse_ptr(50000) == 50000, "recursion through a pointer");
    CHECK(computed_goto(100000) == 25000L * 1111, "computed goto %ld", computed_goto(100000));
    long x30_sum = 0;
    for (int i = 0; i < 1000; i++) x30_sum += call_through_x30(blr_x30_target);
    CHECK(x30_sum == 1000L * 0x5a5a, "BLR x30");
    long x16_sum = 0;
    for (int i = 0; i < 1000; i++) x16_sum += call_ret_via_x16();
    CHECK(x16_sum == 77000, "RET x16 %ld", x16_sum);

    smc_through_pointer();
    signal_during_indirect_loop();
    cross_thread_target_swap();

    if (failures) { printf("INDIRECT_BRANCH_CONTRACT FAIL failures=%d\n", failures); return 1; }
    printf("INDIRECT_BRANCH_CONTRACT PASS\n");
    return 0;
}
