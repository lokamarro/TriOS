// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host issue-ratio measurement.
 *
 * The most transferable result in the RPCS3 talk came from reading the
 * microarchitecture rather than guessing at it: Cortex-A7xx mid-cores issue
 * three 128-bit loads per clock but only two vector operations, which is
 * backwards from every x86 core and inverts what "expensive" means in a hot
 * loop. That number is not ours to reuse. This measures the equivalent ratios
 * on the cores we actually run on, so lowering choices can argue from a model
 * instead of from trial and error.
 *
 * Everything is reported in operations per cycle, and the cycle is measured
 * rather than assumed: a dependent chain of ADDs retires exactly one per cycle
 * on every Arm core of interest, so it calibrates the clock for whichever core
 * class the process was placed on. That matters here because the whole point is
 * to compare P-cores against E-cores, and they do not run at the same
 * frequency.
 *
 * All kernels are 8-way unrolled with independent destinations so the loop
 * measures issue width rather than latency, and every buffer access is inside
 * one 4 KiB page so loads hit L1 and measure the load units rather than the
 * memory system.
 *
 * Build the uml-host-issue-ratio target, then run it once per core class:
 *   bin/uml-host-issue-ratio                            # performance cores
 *   taskpolicy -c background bin/uml-host-issue-ratio   # efficiency cores
 */

#include <pthread.h>
#include <sys/qos.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

#if !defined(__aarch64__)
#error "host_issue_ratio measures AArch64 issue ratios"
#endif

constexpr int unroll = 8;
constexpr int inner_iterations = 4096;
constexpr int repeats = 41;

alignas(4096) static double buffer[512];

static std::uint64_t now_ns()
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/*
 * One dependent ADD per iteration. Its throughput is one per cycle by
 * definition on these cores, so it converts nanoseconds into cycles without
 * relying on a published frequency or on sysctl.
 */
static double dependent_add_ns_per_op()
{
	const std::uint64_t start = now_ns();
	std::uint64_t count = std::uint64_t{inner_iterations} * unroll;
	__asm__ volatile(
		"1:\n\t"
		"add x0, x0, #1\n\t"
		"add x0, x0, #1\n\t"
		"add x0, x0, #1\n\t"
		"add x0, x0, #1\n\t"
		"add x0, x0, #1\n\t"
		"add x0, x0, #1\n\t"
		"add x0, x0, #1\n\t"
		"add x0, x0, #1\n\t"
		"subs %[n], %[n], #8\n\t"
		"b.ne 1b\n\t"
		: [n] "+r"(count)
		:
		: "x0", "cc");
	const std::uint64_t elapsed = now_ns() - start;
	return (double)elapsed / ((double)inner_iterations * unroll);
}

#define LOAD8 \
	"ldr q0, [%[p], #0]\n\t"   "ldr q1, [%[p], #16]\n\t" \
	"ldr q2, [%[p], #32]\n\t"  "ldr q3, [%[p], #48]\n\t" \
	"ldr q4, [%[p], #64]\n\t"  "ldr q5, [%[p], #80]\n\t" \
	"ldr q6, [%[p], #96]\n\t"  "ldr q7, [%[p], #112]\n\t"

/*
 * Accumulator count is a correctness property here, not a style choice. Each
 * fadd depends on its own previous result, so N accumulators sustain at most
 * N / latency operations per cycle. FADD latency is around 3 cycles on these
 * cores, so an 8-accumulator kernel tops out near 2.7/cycle and reports latency
 * dressed up as issue width - which is exactly what the first version of this
 * file measured. Sixteen independent chains clear four per cycle with margin.
 */
#define VALU16 \
	"fadd v8.2d, v8.2d, v24.2d\n\t"   "fadd v9.2d, v9.2d, v24.2d\n\t" \
	"fadd v10.2d, v10.2d, v24.2d\n\t" "fadd v11.2d, v11.2d, v24.2d\n\t" \
	"fadd v12.2d, v12.2d, v24.2d\n\t" "fadd v13.2d, v13.2d, v24.2d\n\t" \
	"fadd v14.2d, v14.2d, v24.2d\n\t" "fadd v15.2d, v15.2d, v24.2d\n\t" \
	"fadd v16.2d, v16.2d, v24.2d\n\t" "fadd v17.2d, v17.2d, v24.2d\n\t" \
	"fadd v18.2d, v18.2d, v24.2d\n\t" "fadd v19.2d, v19.2d, v24.2d\n\t" \
	"fadd v20.2d, v20.2d, v24.2d\n\t" "fadd v21.2d, v21.2d, v24.2d\n\t" \
	"fadd v22.2d, v22.2d, v24.2d\n\t" "fadd v23.2d, v23.2d, v24.2d\n\t"

#define VALU8 \
	"fadd v8.2d, v8.2d, v24.2d\n\t"   "fadd v9.2d, v9.2d, v24.2d\n\t" \
	"fadd v10.2d, v10.2d, v24.2d\n\t" "fadd v11.2d, v11.2d, v24.2d\n\t" \
	"fadd v12.2d, v12.2d, v24.2d\n\t" "fadd v13.2d, v13.2d, v24.2d\n\t" \
	"fadd v14.2d, v14.2d, v24.2d\n\t" "fadd v15.2d, v15.2d, v24.2d\n\t"

#define LOAD4 \
	"ldr q0, [%[p], #0]\n\t"  "ldr q1, [%[p], #16]\n\t" \
	"ldr q2, [%[p], #32]\n\t" "ldr q3, [%[p], #48]\n\t"

#define VALU4 \
	"fadd v8.2d, v8.2d, v24.2d\n\t"   "fadd v9.2d, v9.2d, v24.2d\n\t" \
	"fadd v10.2d, v10.2d, v24.2d\n\t" "fadd v11.2d, v11.2d, v24.2d\n\t"

#define KERNEL(name, body, ops_per_iteration)                              \
	static double name()                                           \
	{                                                                  \
		const double *p = buffer;                                  \
		std::uint64_t count = inner_iterations;                         \
		const std::uint64_t start = now_ns();                           \
		__asm__ volatile(                                          \
			"1:\n\t" body                                      \
			"subs %[n], %[n], #1\n\t"                          \
			"b.ne 1b\n\t"                                      \
			: [n] "+r"(count)                                  \
			: [p] "r"(p)                                       \
			: "cc", "v0", "v1", "v2", "v3", "v4", "v5", "v6",  \
			  "v7", "v8", "v9", "v10", "v11", "v12", "v13",    \
			  "v14", "v15", "v16", "v17", "v18", "v19", "v20", \
			  "v21", "v22", "v23", "memory");                  \
		const std::uint64_t elapsed = now_ns() - start;                 \
		return (double)elapsed /                                   \
			((double)inner_iterations * (ops_per_iteration));  \
	}

KERNEL(loads_only, LOAD8, 8)
KERNEL(valu_only, VALU16, 16)
KERNEL(valu_only_8, VALU8, 8)
KERNEL(mixed_1_1, LOAD4 VALU4, 8)
KERNEL(mixed_2_1, LOAD8 VALU4, 12)
KERNEL(mixed_3_2, LOAD8 LOAD4 VALU8, 20)
KERNEL(mixed_1_2, LOAD4 VALU8, 12)

int main()
{
	/* Without an explicit class the scheduler is free to move this process
	 * between core classes mid-run, and it does. The first version of this
	 * file calibrated at 2.06 GHz and then reported 6.06 vector ops per cycle,
	 * which no Apple core can issue - it had been calibrated on an efficiency
	 * core and measured on a performance one. Asking for user-interactive
	 * keeps it on performance cores; `taskpolicy -c background` still wins for
	 * the efficiency-core run, which is what makes both halves measurable. */
	if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) != 0)
		fprintf(stderr, "warning: could not set QoS class\n");

	for (size_t index = 0; index < sizeof(buffer) / sizeof(buffer[0]);
	     ++index)
		buffer[index] = (double)index;

	/* Warm the frequency governor and the caches before anything counts. */
	for (int index = 0; index < 200; ++index) {
		(void)dependent_add_ns_per_op();
		(void)loads_only();
		(void)valu_only();
	}

	const struct {
		const char *name;
		double (*kernel)(void);
	} kernels[] = {
		{"loads only (128-bit)", loads_only},
		{"vector ALU only (16 chains)", valu_only},
		{"vector ALU only (8 chains)", valu_only_8},
		{"mixed 1 load : 1 valu", mixed_1_1},
		{"mixed 2 load : 1 valu", mixed_2_1},
		{"mixed 3 load : 2 valu", mixed_3_2},
		{"mixed 1 load : 2 valu", mixed_1_2},
	};

	printf("%-28s %11s %8s\n", "kernel", "ops/cycle", "GHz");
	for (size_t index = 0; index < sizeof(kernels) / sizeof(kernels[0]);
	     ++index) {
		/* Calibrate against each kernel rather than once up front, so a
		 * migration shows up as a changed frequency beside the result it
		 * would otherwise silently corrupt. */
		std::array<double, repeats> cycles{};
		std::array<double, repeats> ops{};
		for (int sample = 0; sample < repeats; ++sample) {
			cycles[sample] = dependent_add_ns_per_op();
			ops[sample] = kernels[index].kernel();
		}
		std::sort(cycles.begin(), cycles.end());
		std::sort(ops.begin(), ops.end());
		const double cycle_ns = cycles[repeats / 2];
		printf("%-28s %11.3f %8.2f\n", kernels[index].name,
			cycle_ns / ops[repeats / 2], 1.0 / cycle_ns);
	}
	return 0;
}
