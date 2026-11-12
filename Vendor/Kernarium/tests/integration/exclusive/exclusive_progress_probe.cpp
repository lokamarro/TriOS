// SPDX-License-Identifier: GPL-2.0-or-later
//
// Freestanding arm64/Linux guest probe: a load/store-exclusive loop makes
// progress whatever the software TLB holds.
//
// The loop is the one ART's MessageStack.pushMessage compiles to: read the
// top of a stack, store it into a new message, then publish the message with
// a weak compare-and-swap (one LDXR/STLXR attempt, retried by the caller).
// When the message's page and the stack's page share a TLB entry, the plain
// store evicts the target's write entry, so a store-exclusive that depends on
// the TLB loses the host monitor on every pass and never completes.
//
// The TLB has 16384 direct-mapped entries: entry = key & 0x3fff, where
// key = address >> 12. The pages below are chosen by that function, so a
// change of geometry shows as the collision tests no longer colliding in
// tests/unit/arm64/arm64_exclusive_progress_tests.cpp, which guards them.
//
// With the argument "inside" the loops also store between the exclusive load
// and the store-exclusive, to another page. The architecture does not promise
// progress there, but both engines keep the monitor across a store of the
// guest's own, as a register spill between the pair needs.
//
// A copy-on-write break between the pair is covered by running the loop in
// a forked child on a page it shares with its parent.

namespace {

using u64 = unsigned long;
using u32 = unsigned int;
using u16 = unsigned short;
using u8 = unsigned char;

constexpr long nr_write = 64;
constexpr long nr_exit_group = 94;
constexpr long nr_clone = 220;
constexpr long nr_mmap = 222;
constexpr long nr_wait4 = 260;
constexpr long sigchld = 17;

inline long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0,
			long a3 = 0, long a4 = 0, long a5 = 0)
{
	register long x8 __asm__("x8") = number;
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x4 __asm__("x4") = a4;
	register long x5 __asm__("x5") = a5;
	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory", "cc");
	return x0;
}

unsigned long length(const char *s)
{
	unsigned long n = 0;
	while (s[n])
		n++;
	return n;
}

void out(const char *s)
{
	raw_syscall(nr_write, 1, reinterpret_cast<long>(s),
		    static_cast<long>(length(s)));
}

void out_num(long value)
{
	char buffer[32];
	int at = 31;
	buffer[at] = 0;
	unsigned long v = value < 0 ? -value : value;
	do {
		buffer[--at] = static_cast<char>('0' + v % 10);
		v /= 10;
	} while (v);
	if (value < 0)
		buffer[--at] = '-';
	out(buffer + at);
}

// One compare-and-swap attempt of each form. `Store` is a plain store that
// the "inside" shape puts between the pair; the others make it before the
// load, in the caller. Each returns whether the store-exclusive succeeded.
#define WEAK_CAS(NAME, LOAD, STORE, REGISTER, TYPE, VALUE)                     \
	inline bool NAME(volatile TYPE *address, VALUE expected,               \
			 VALUE desired, volatile u32 *between)                 \
	{                                                                      \
		VALUE observed;                                                \
		u32 status;                                                    \
		if (between) {                                                 \
			__asm__ volatile(LOAD " %" REGISTER "0, [%2]\n"        \
					 "cmp %" REGISTER "0, %" REGISTER "3\n" \
					 "b.ne 1f\n"                           \
					 "str wzr, [%5]\n"                     \
					 STORE " %w1, %" REGISTER "4, [%2]\n"  \
					 "b 2f\n"                              \
					 "1: mov %w1, #1\n"                    \
					 "2:\n"                                \
					 : "=&r"(observed), "=&r"(status)      \
					 : "r"(address), "r"(expected),        \
					   "r"(desired), "r"(between)          \
					 : "memory", "cc");                    \
		} else {                                                       \
			__asm__ volatile(LOAD " %" REGISTER "0, [%2]\n"        \
					 "cmp %" REGISTER "0, %" REGISTER "3\n" \
					 "b.ne 1f\n"                           \
					 STORE " %w1, %" REGISTER "4, [%2]\n"  \
					 "b 2f\n"                              \
					 "1: mov %w1, #1\n"                    \
					 "2:\n"                                \
					 : "=&r"(observed), "=&r"(status)      \
					 : "r"(address), "r"(expected),        \
					   "r"(desired)                        \
					 : "memory", "cc");                    \
		}                                                              \
		return status == 0;                                            \
	}

// ART's weakCompareAndSetRelease is the 32-bit LDXR/STLXR form.
WEAK_CAS(cas_w_release, "ldxr", "stlxr", "w", u32, u32)
WEAK_CAS(cas_w_acquire_release, "ldaxr", "stlxr", "w", u32, u32)
WEAK_CAS(cas_x_relaxed, "ldxr", "stxr", "x", u64, u64)
WEAK_CAS(cas_x_acquire_release, "ldaxr", "stlxr", "x", u64, u64)
WEAK_CAS(cas_h_release, "ldxrh", "stlxrh", "w", u16, u32)
WEAK_CAS(cas_b_relaxed, "ldxrb", "stxrb", "w", u8, u32)

// The pair form compares both halves.
inline bool cas_pair(volatile u64 *address, u64 expected_low, u64 expected_high,
		     u64 desired_low, u64 desired_high, volatile u32 *between,
		     bool acquire_release)
{
	u64 low, high;
	u32 status;
	if (acquire_release) {
		if (between) {
			__asm__ volatile("ldaxp %0, %1, [%3]\n"
					 "cmp %0, %4\n"
					 "ccmp %1, %5, #0, eq\n"
					 "b.ne 1f\n"
					 "str wzr, [%8]\n"
					 "stlxp %w2, %6, %7, [%3]\n"
					 "b 2f\n"
					 "1: mov %w2, #1\n"
					 "2:\n"
					 : "=&r"(low), "=&r"(high), "=&r"(status)
					 : "r"(address), "r"(expected_low),
					   "r"(expected_high), "r"(desired_low),
					   "r"(desired_high), "r"(between)
					 : "memory", "cc");
		} else {
			__asm__ volatile("ldaxp %0, %1, [%3]\n"
					 "cmp %0, %4\n"
					 "ccmp %1, %5, #0, eq\n"
					 "b.ne 1f\n"
					 "stlxp %w2, %6, %7, [%3]\n"
					 "b 2f\n"
					 "1: mov %w2, #1\n"
					 "2:\n"
					 : "=&r"(low), "=&r"(high), "=&r"(status)
					 : "r"(address), "r"(expected_low),
					   "r"(expected_high), "r"(desired_low),
					   "r"(desired_high)
					 : "memory", "cc");
		}
	} else {
		if (between) {
			__asm__ volatile("ldxp %0, %1, [%3]\n"
					 "cmp %0, %4\n"
					 "ccmp %1, %5, #0, eq\n"
					 "b.ne 1f\n"
					 "str wzr, [%8]\n"
					 "stxp %w2, %6, %7, [%3]\n"
					 "b 2f\n"
					 "1: mov %w2, #1\n"
					 "2:\n"
					 : "=&r"(low), "=&r"(high), "=&r"(status)
					 : "r"(address), "r"(expected_low),
					   "r"(expected_high), "r"(desired_low),
					   "r"(desired_high), "r"(between)
					 : "memory", "cc");
		} else {
			__asm__ volatile("ldxp %0, %1, [%3]\n"
					 "cmp %0, %4\n"
					 "ccmp %1, %5, #0, eq\n"
					 "b.ne 1f\n"
					 "stxp %w2, %6, %7, [%3]\n"
					 "b 2f\n"
					 "1: mov %w2, #1\n"
					 "2:\n"
					 : "=&r"(low), "=&r"(high), "=&r"(status)
					 : "r"(address), "r"(expected_low),
					   "r"(expected_high), "r"(desired_low),
					   "r"(desired_high)
					 : "memory", "cc");
		}
	}
	return status == 0;
}

enum class Form {
	w_release,
	w_acquire_release,
	x_relaxed,
	x_acquire_release,
	h_release,
	b_relaxed,
	pair_relaxed,
	pair_acquire_release,
};

constexpr Form forms[] = {
	Form::w_release,	  Form::w_acquire_release, Form::x_relaxed,
	Form::x_acquire_release, Form::h_release,	  Form::b_relaxed,
	Form::pair_relaxed,	  Form::pair_acquire_release,
};

const char *name_of(Form form)
{
	switch (form) {
	case Form::w_release:
		return "ldxr/stlxr w";
	case Form::w_acquire_release:
		return "ldaxr/stlxr w";
	case Form::x_relaxed:
		return "ldxr/stxr x";
	case Form::x_acquire_release:
		return "ldaxr/stlxr x";
	case Form::h_release:
		return "ldxrh/stlxrh";
	case Form::b_relaxed:
		return "ldxrb/stxrb";
	case Form::pair_relaxed:
		return "ldxp/stxp";
	case Form::pair_acquire_release:
		return "ldaxp/stlxp";
	}
	return "";
}

// One attempt of `form` against the 16-byte-aligned stack top: expects
// `current` (and, for a pair, its successor) and installs `desired`.
bool attempt(Form form, volatile void *top, u64 current, u64 desired,
	     volatile u32 *between)
{
	switch (form) {
	case Form::w_release:
		return cas_w_release(static_cast<volatile u32 *>(top),
				     static_cast<u32>(current),
				     static_cast<u32>(desired), between);
	case Form::w_acquire_release:
		return cas_w_acquire_release(static_cast<volatile u32 *>(top),
					     static_cast<u32>(current),
					     static_cast<u32>(desired), between);
	case Form::x_relaxed:
		return cas_x_relaxed(static_cast<volatile u64 *>(top), current,
				     desired, between);
	case Form::x_acquire_release:
		return cas_x_acquire_release(static_cast<volatile u64 *>(top),
					     current, desired, between);
	case Form::h_release:
		return cas_h_release(static_cast<volatile u16 *>(top),
				     static_cast<u32>(current & 0xffff),
				     static_cast<u32>(desired & 0xffff), between);
	case Form::b_relaxed:
		return cas_b_relaxed(static_cast<volatile u8 *>(top),
				     static_cast<u32>(current & 0xff),
				     static_cast<u32>(desired & 0xff), between);
	case Form::pair_relaxed:
		return cas_pair(static_cast<volatile u64 *>(top), current,
				current + 1, desired, desired + 1, between, false);
	case Form::pair_acquire_release:
		return cas_pair(static_cast<volatile u64 *>(top), current,
				current + 1, desired, desired + 1, between, true);
	}
	return false;
}

u64 read_top(Form form, volatile void *top)
{
	switch (form) {
	case Form::w_release:
	case Form::w_acquire_release:
		return __atomic_load_n(static_cast<volatile u32 *>(top),
				       __ATOMIC_ACQUIRE);
	case Form::h_release:
		return __atomic_load_n(static_cast<volatile u16 *>(top),
				       __ATOMIC_ACQUIRE);
	case Form::b_relaxed:
		return __atomic_load_n(static_cast<volatile u8 *>(top),
				       __ATOMIC_ACQUIRE);
	default:
		return __atomic_load_n(static_cast<volatile u64 *>(top),
				       __ATOMIC_ACQUIRE);
	}
}

void set_top(Form form, volatile void *top, u64 value)
{
	switch (form) {
	case Form::w_release:
	case Form::w_acquire_release:
		*static_cast<volatile u32 *>(top) = static_cast<u32>(value);
		break;
	case Form::h_release:
		*static_cast<volatile u16 *>(top) = static_cast<u16>(value);
		break;
	case Form::b_relaxed:
		*static_cast<volatile u8 *>(top) = static_cast<u8>(value);
		break;
	case Form::pair_relaxed:
	case Form::pair_acquire_release:
		static_cast<volatile u64 *>(top)[0] = value;
		static_cast<volatile u64 *>(top)[1] = value + 1;
		break;
	default:
		*static_cast<volatile u64 *>(top) = value;
		break;
	}
}

// Run `rounds` pushes of `form`. A push stores the old top into each message
// page, then publishes. Returns the number of pushes that needed `limit`
// attempts or more.
long run(Form form, volatile void *top, volatile u32 *message_one,
	 volatile u32 *message_two, volatile u32 *between, long rounds,
	 long limit, long &worst)
{
	long stuck = 0;
	u64 next_value = 2;
	worst = 0;
	for (long round = 0; round < rounds; round++) {
		long attempts = 0;
		for (;;) {
			const u64 current = read_top(form, top);
			message_one[4] = static_cast<u32>(current);
			if (message_two)
				message_two[4] = static_cast<u32>(current);
			attempts++;
			if (attempt(form, top, current, next_value, between))
				break;
			if (attempts >= limit)
				break;
		}
		next_value++;
		if (attempts >= limit)
			stuck++;
		if (attempts > worst)
			worst = attempts;
	}
	return stuck;
}

void *map_fixed(u64 address, long page)
{
	// PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE
	long result = raw_syscall(nr_mmap, static_cast<long>(address), page, 3,
				  0x2 | 0x20 | 0x100000, -1, 0);
	return result < 0 && result > -4096 ? nullptr
					    : reinterpret_cast<void *>(result);
}

u64 tlb_entry(u64 key, long shift)
{
	(void)shift;
	return key & 0x3fff;
}

// The `ordinal`-th key above `target` that shares the target's TLB entry.
u64 key_sharing_entry(u64 target, long ordinal, long shift)
{
	long seen = 0;
	for (u64 key = target + 1;; key++) {
		if (tlb_entry(key, shift) != tlb_entry(target, shift))
			continue;
		if (++seen == ordinal)
			return key;
	}
}

// A key above `target` in another entry.
u64 key_apart(u64 target, long shift)
{
	u64 key = target + 1;
	while (tlb_entry(key, shift) == tlb_entry(target, shift))
		key++;
	return key;
}

[[noreturn]] void finish(long failures)
{
	out(failures ? "UML_EXCLUSIVE_PROGRESS: FAIL\n"
		     : "UML_EXCLUSIVE_PROGRESS: PASS\n");
	raw_syscall(nr_exit_group, failures ? 1 : 0);
	__builtin_unreachable();
}

long report(const char *shape, Form form, long stuck, long worst)
{
	out(shape);
	out(" ");
	out(name_of(form));
	out(": stuck=");
	out_num(stuck);
	out(" worst_attempts=");
	out_num(worst);
	out("\n");
	return stuck != 0;
}

}  // namespace

extern "C" __attribute__((noreturn, used)) void probe_main(long *sp)
{
	long argc = *sp;
	char **argv = reinterpret_cast<char **>(sp + 1);
	bool inside = argc > 1 && argv[1][0] == 'i';
	constexpr long page = 4096;
	constexpr long shift = 12;

	const u64 key_top = 0x100000 >> (shift - 12);
	const u64 key_apart_page = key_apart(key_top, shift);
	const u64 key_one = key_sharing_entry(key_top, 1, shift);
	const u64 key_two = key_sharing_entry(key_top, 2, shift);
	void *page_top = map_fixed(key_top << shift, page);
	void *page_apart = map_fixed(key_apart_page << shift, page);
	void *page_one = map_fixed(key_one << shift, page);
	void *page_two = map_fixed(key_two << shift, page);
	if (!page_top || !page_apart || !page_one || !page_two) {
		out("mmap failed\n");
		raw_syscall(nr_exit_group, 2);
	}
	out("page size ");
	out_num(page);
	out(inside ? ", stores between the pair\n" : "\n");

	// A cold word of the top page keeps the loops off its page boundary and
	// the 16-byte alignment the pair form needs.
	volatile void *top = static_cast<volatile char *>(page_top) + 0x40;
	// The store between the pair goes to a page of the same entry.
	volatile u32 *between = inside ? static_cast<volatile u32 *>(page_two) : nullptr;
	long failures = 0;
	long worst = 0;

	for (const Form form : forms) {
		set_top(form, top, 1);
		failures += report("apart     ", form,
				   run(form, top, static_cast<volatile u32 *>(page_apart),
				       nullptr, nullptr, 2000, 100000, worst),
				   worst);
	}
	// Both colliding shapes: one message page, then two (a third page
	// contending for the same entry).
	for (const Form form : forms) {
		set_top(form, top, 1);
		failures += report("colliding ", form,
				   run(form, top, static_cast<volatile u32 *>(page_one),
				       nullptr, between, 20, 5000, worst),
				   worst);
		set_top(form, top, 1);
		failures += report("colliding3", form,
				   run(form, top, static_cast<volatile u32 *>(page_one),
				       static_cast<volatile u32 *>(page_two), between,
				       20, 5000, worst),
				   worst);
	}

	// A copy-on-write break between the pair: the child shares the top page
	// with the parent, so its store-exclusive faults, the kernel copies the
	// page, and the loop continues on the copy.
	for (const Form form : forms) {
		set_top(form, top, 1);
		long child = raw_syscall(nr_clone, sigchld);
		if (child < 0) {
			out("clone failed\n");
			failures++;
			continue;
		}
		if (child == 0) {
			long child_worst = 0;
			long stuck = run(form, top, static_cast<volatile u32 *>(page_one),
					 nullptr, between, 20, 5000, child_worst);
			raw_syscall(nr_exit_group, stuck ? 1 : 0);
		}
		int status = -1;
		raw_syscall(nr_wait4, child, reinterpret_cast<long>(&status), 0, 0);
		out("cow       ");
		out(name_of(form));
		out(": child status ");
		out_num(status);
		out("\n");
		if (status != 0)
			failures++;
	}
	finish(failures);
}

__asm__(".text\n"
	".global _start\n"
	"_start:\n"
	"	mov x0, sp\n"
	"	mov x1, sp\n"
	"	and x1, x1, #-16\n"
	"	mov sp, x1\n"
	"	bl probe_main\n");
