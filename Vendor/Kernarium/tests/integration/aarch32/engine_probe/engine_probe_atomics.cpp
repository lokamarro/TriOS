// SPDX-License-Identifier: GPL-2.0-or-later
/* Group "atomics": the exclusives of every width and the kernel's compare and
 * swap helpers under contention, from A32 and from T32, across the threads of
 * one process, across processes sharing a page, with plain stores on the same
 * page (the exclusive pair has to make forward progress), and with the
 * ordering a DMB gives between a data word and its flag. A count that is
 * short means a lost update; a thread that does not finish in the deadline
 * means the engine's exclusives livelock. While abi.swp is 1 (or can be set
 * to it) SWP and SWPB, which the kernel emulates with one atomic exchange,
 * take part: threads and processes hand tokens through one word with SWP,
 * with LDREX/STREX from A32 and T32 and with the kernel's cmpxchg, and the
 * tokens that come out must be the ones that went in; SWPB on one byte of a
 * word must leave the LDREXB counters on its neighbours exact. */
#include "engine_probe.hpp"

extern "C" {
#define AT_FUNCTIONS(isa)                                                     \
	probe::u32 at_add_ldrex_##isa(probe::u32 *p, probe::u32 n);          \
	probe::u32 at_add_ldrexb_##isa(probe::u8 *p, probe::u32 n);          \
	probe::u32 at_add_ldrexh_##isa(probe::u16 *p, probe::u32 n);         \
	void at_add_ldrexd_##isa(probe::u64 *p, probe::u32 n);               \
	probe::u32 at_add_cmpxchg_##isa(probe::u32 *p, probe::u32 n);        \
	void at_add_cmpxchg64_##isa(probe::u64 *p, probe::u32 n);            \
	void at_store_loop_##isa(volatile probe::u32 *p, volatile probe::u32 *stop); \
	void at_pair_writer_##isa(probe::u64 *p, probe::u32 n);              \
	probe::u32 at_pair_reader_ldrexd_##isa(probe::u64 *p, volatile probe::u32 *stop); \
	probe::u32 at_pair_reader_ldrd_##isa(probe::u64 *p, volatile probe::u32 *stop);   \
	void at_monitor_##isa(probe::u32 *p, probe::u32 *out);                \
	probe::u32 at_swap_ldrex_##isa(probe::u32 *p, probe::u32 token, probe::u32 n);    \
	probe::u32 at_swap_cmpxchg_##isa(probe::u32 *p, probe::u32 token, probe::u32 n);
AT_FUNCTIONS(arm)
AT_FUNCTIONS(thumb)
#undef AT_FUNCTIONS
/* A32 only. */
probe::u32 at_swap_swp_arm(probe::u32 *p, probe::u32 token, probe::u32 n);
probe::u32 at_swap_swpb_arm(probe::u8 *p, probe::u32 token, probe::u32 n);
}

namespace probe {
namespace {

enum Kind : u32 {
	add_word,
	add_byte,
	add_half,
	add_double,
	add_cmpxchg,
	add_cmpxchg64,
	store_loop,
	pair_writer,
	pair_reader_ldrexd,
	pair_reader_ldrd,
	swap_swp,
	swap_swpb,
	swap_ldrex,
	swap_cmpxchg,
};

struct Work {
	Kind kind;
	bool thumb;
	void *target;
	u32 count;
	volatile u32 *go;
	volatile u32 *stop;
	u32 result;
	/* The token a swap thread starts with. */
	u32 token = 0;
};

long worker(void *argument)
{
	auto *work = static_cast<Work *>(argument);
	while (!*work->go)
		yield();
	const bool t = work->thumb;
	switch (work->kind) {
	case add_word:
		work->result = t ? at_add_ldrex_thumb(static_cast<u32 *>(work->target), work->count) :
				   at_add_ldrex_arm(static_cast<u32 *>(work->target), work->count);
		break;
	case add_byte:
		work->result = t ? at_add_ldrexb_thumb(static_cast<u8 *>(work->target), work->count) :
				   at_add_ldrexb_arm(static_cast<u8 *>(work->target), work->count);
		break;
	case add_half:
		work->result = t ? at_add_ldrexh_thumb(static_cast<u16 *>(work->target), work->count) :
				   at_add_ldrexh_arm(static_cast<u16 *>(work->target), work->count);
		break;
	case add_double:
		if (t)
			at_add_ldrexd_thumb(static_cast<u64 *>(work->target), work->count);
		else
			at_add_ldrexd_arm(static_cast<u64 *>(work->target), work->count);
		break;
	case add_cmpxchg:
		work->result = t ? at_add_cmpxchg_thumb(static_cast<u32 *>(work->target), work->count) :
				   at_add_cmpxchg_arm(static_cast<u32 *>(work->target), work->count);
		break;
	case add_cmpxchg64:
		if (t)
			at_add_cmpxchg64_thumb(static_cast<u64 *>(work->target), work->count);
		else
			at_add_cmpxchg64_arm(static_cast<u64 *>(work->target), work->count);
		break;
	case store_loop:
		if (t)
			at_store_loop_thumb(static_cast<volatile u32 *>(work->target), work->stop);
		else
			at_store_loop_arm(static_cast<volatile u32 *>(work->target), work->stop);
		break;
	case pair_writer:
		if (t)
			at_pair_writer_thumb(static_cast<u64 *>(work->target), work->count);
		else
			at_pair_writer_arm(static_cast<u64 *>(work->target), work->count);
		break;
	case pair_reader_ldrexd:
		work->result = t ? at_pair_reader_ldrexd_thumb(static_cast<u64 *>(work->target), work->stop) :
				   at_pair_reader_ldrexd_arm(static_cast<u64 *>(work->target), work->stop);
		break;
	case pair_reader_ldrd:
		work->result = t ? at_pair_reader_ldrd_thumb(static_cast<u64 *>(work->target), work->stop) :
				   at_pair_reader_ldrd_arm(static_cast<u64 *>(work->target), work->stop);
		break;
	case swap_swp:
		work->result = at_swap_swp_arm(static_cast<u32 *>(work->target), work->token, work->count);
		break;
	case swap_swpb:
		work->result = at_swap_swpb_arm(static_cast<u8 *>(work->target), work->token, work->count);
		break;
	case swap_ldrex:
		work->result = t ? at_swap_ldrex_thumb(static_cast<u32 *>(work->target), work->token, work->count) :
				   at_swap_ldrex_arm(static_cast<u32 *>(work->target), work->token, work->count);
		break;
	case swap_cmpxchg:
		work->result = t ? at_swap_cmpxchg_thumb(static_cast<u32 *>(work->target), work->token, work->count) :
				   at_swap_cmpxchg_arm(static_cast<u32 *>(work->target), work->token, work->count);
		break;
	}
	return 0;
}

constexpr u32 deadline_seconds = 120;
constexpr u32 rounds = 20000;

/* Starts a thread for each of work[0..count), releases them together and
 * joins them. `wait_for` is how many of them have to finish before the others
 * are told to stop (the others run until `stop` is set, such as a plain store
 * loop); a thread that misses the deadline ends the group. */
void run_workers(Work *work, unsigned count, volatile u32 *go, volatile u32 *stop, unsigned finite,
		 const char *what)
{
	Thread threads[8];
	*go = 0;
	*stop = 0;
	for (unsigned index = 0; index < count; ++index) {
		work[index].go = go;
		work[index].stop = stop;
		work[index].result = 0;
		if (!thread_start(threads[index], worker, &work[index])) {
			check(false, "clone starts a thread");
			end_group();
		}
	}
	*go = 1;
	/* The finite workers come first in the array. */
	for (unsigned index = 0; index < finite; ++index) {
		if (!thread_join(threads[index], deadline_seconds)) {
			put("FAIL ");
			put(group_name);
			put("/");
			put(what);
			put(": a thread did not finish in the deadline (livelock)\n");
			++stats.checks;
			++stats.failures;
			end_group();
		}
	}
	*stop = 1;
	for (unsigned index = finite; index < count; ++index) {
		if (!thread_join(threads[index], deadline_seconds)) {
			check(false, "a background thread stops");
			end_group();
		}
	}
}

} // namespace

void group_atomics()
{
	const word region_length = 1 << 16;
	const word region = mmap_pages(region_length, prot_read | prot_write);
	check(region < static_cast<word>(-4096), "a private page is mapped");
	const word shared_region = mmap_pages(4096, prot_read | prot_write, map_shared | map_anonymous);
	check(shared_region < static_cast<word>(-4096), "a shared page is mapped");
	/* Control flags live on their own page. */
	auto *control = ptr<volatile u32>(shared_region);
	volatile u32 *go = control;
	volatile u32 *stop = control + 16;

	/* The exclusive monitor: STREX after LDREX stores, a second fails, and
	 * so does one after CLREX. Hardware may fail the first STREX
	 * spuriously too, as an interrupt or a preemption of the host thread
	 * does here, so it must succeed within a few attempts, and an attempt
	 * whose first STREX failed must have stored nothing. */
	for (int thumb = 0; thumb < 2; ++thumb) {
		auto *word_cell = ptr<u32>(region);
		const unsigned attempts = 16;
		u32 out[3] = {99, 99, 99};
		for (unsigned attempt = 0; attempt < attempts; ++attempt) {
			*word_cell = 10;
			out[0] = out[1] = out[2] = 99;
			if (thumb)
				at_monitor_thumb(word_cell, out);
			else
				at_monitor_arm(word_cell, out);
			if (out[0] == 0)
				break;
			check_eq(out[0], 1, thumb ? "T32: a STREX's status is 0 or 1" : "A32: a STREX's status is 0 or 1");
			check_eq(*word_cell, 10, thumb ? "T32: a failed STREX stores nothing" : "A32: a failed STREX stores nothing");
		}
		check_eq(out[0], 0, thumb ? "T32: STREX after LDREX returns 0" : "A32: STREX after LDREX returns 0");
		check_eq(out[1], 1, thumb ? "T32: a second STREX fails" : "A32: a second STREX fails");
		check_eq(out[2], 1, thumb ? "T32: STREX after CLREX fails" : "A32: STREX after CLREX fails");
		check_eq(*word_cell, 11, thumb ? "T32: only the first store happened" : "A32: only the first store happened");
	}

	/* One counter; every width and helper from threads of both instruction
	 * sets. */
	{
		auto *counter = ptr<u32>(region + 256);
		*counter = 0;
		Work work[5] = {
			{add_word, false, counter, rounds, nullptr, nullptr, 0},
			{add_word, false, counter, rounds, nullptr, nullptr, 0},
			{add_word, true, counter, rounds, nullptr, nullptr, 0},
			{add_word, true, counter, rounds, nullptr, nullptr, 0},
			{add_word, false, counter, rounds, nullptr, nullptr, 0},
		};
		run_workers(work, 5, go, stop, 5, "LDREX/STREX counter");
		check_eq(*counter, 5 * rounds, "five threads of LDREX/STREX (A32 and T32) lose no update");
	}
	{
		auto *counter = ptr<u32>(region + 512);
		*counter = 0;
		Work work[4] = {
			{add_cmpxchg, false, counter, rounds, nullptr, nullptr, 0},
			{add_cmpxchg, true, counter, rounds, nullptr, nullptr, 0},
			{add_cmpxchg, false, counter, rounds, nullptr, nullptr, 0},
			{add_cmpxchg, true, counter, rounds, nullptr, nullptr, 0},
		};
		run_workers(work, 4, go, stop, 4, "kuser cmpxchg counter");
		check_eq(*counter, 4 * rounds, "four threads of the kuser cmpxchg (A32 and T32) lose no update");
	}
	{
		/* The two kinds on one word: each must see the other's stores. */
		auto *counter = ptr<u32>(region + 768);
		*counter = 0;
		Work work[6] = {
			{add_word, false, counter, rounds, nullptr, nullptr, 0},
			{add_cmpxchg, false, counter, rounds, nullptr, nullptr, 0},
			{add_word, true, counter, rounds, nullptr, nullptr, 0},
			{add_cmpxchg, true, counter, rounds, nullptr, nullptr, 0},
			{add_word, false, counter, rounds, nullptr, nullptr, 0},
			{add_cmpxchg, false, counter, rounds, nullptr, nullptr, 0},
		};
		run_workers(work, 6, go, stop, 6, "mixed counter");
		check_eq(*counter, 6 * rounds, "LDREX/STREX and the kuser cmpxchg on one word lose no update");
	}
	/* SWP and SWPB beside the exclusives. The kernel emulates them, so each
	 * one is a trap and far slower than a loop of LDREX/STREX: the slow
	 * threads do a few thousand exchanges, the fast ones enough to run
	 * beside them. */
	bool swp_ready = false;
	if (under_qemu()) {
		swp_ready = true;
	} else {
		const long setting = swp_setting();
		swp_ready = setting == 1 || (setting == 0 && swp_set(1));
		if (!swp_ready)
			line("NOTE atomics: abi.swp is 0 and not writable by this process; the SWP checks are skipped");
	}
	if (swp_ready) {
		constexpr u32 trap_rounds = 8000;
		constexpr u32 fast_rounds = 1000000;
		/* Tokens, one bit each: the word starts with bit 0 and each thread
		 * with its own. Each swaps its token for the word's, so at the end
		 * the word and the threads hold the bits between them, each once.
		 * First SWP beside LDREX/STREX pairs from A32 and T32, whose pair
		 * is adjacent, then SWP beside the kernel's cmpxchg helper, whose
		 * LDREX and STREX have a comparison between them. */
		struct Mix {
			const char *name;
			Kind kind;
		};
		static const Mix mixes[] = {
			{"SWP and LDREX/STREX handing tokens through one word lose and duplicate none", swap_ldrex},
			{"SWP and the kuser cmpxchg handing tokens through one word lose and duplicate none",
			 swap_cmpxchg},
		};
		auto *cell = ptr<u32>(region + 3072);
		for (const Mix &mix : mixes) {
			*cell = 1U << 0;
			Work work[4] = {
				{swap_swp, false, cell, trap_rounds, nullptr, nullptr, 0, 1U << 1},
				{swap_swp, false, cell, trap_rounds, nullptr, nullptr, 0, 1U << 2},
				{mix.kind, false, cell, fast_rounds, nullptr, nullptr, 0, 1U << 3},
				{mix.kind, true, cell, fast_rounds, nullptr, nullptr, 0, 1U << 4},
			};
			run_workers(work, 4, go, stop, 4, "SWP token exchange");
			u32 sum = *cell, bits = *cell;
			for (const Work &item : work) {
				sum += item.result;
				bits |= item.result;
			}
			check_eq(bits, 0x1f, mix.name);
			if (bits == 0x1f)
				check_eq(sum, 0x1f, "and no token is held twice");
		}

		/* SWPB on one byte beside LDREXB counters on the next. */
		auto *bytes = ptr<u8>(region + 3328);
		bytes[0] = 0x04;
		bytes[1] = bytes[2] = bytes[3] = 0;
		constexpr u32 counter_rounds = 500000;
		Work byte_work[4] = {
			{swap_swpb, false, bytes + 0, trap_rounds, nullptr, nullptr, 0, 0x01},
			{swap_swpb, false, bytes + 0, trap_rounds, nullptr, nullptr, 0, 0x02},
			{add_byte, false, bytes + 1, counter_rounds, nullptr, nullptr, 0},
			{add_byte, true, bytes + 1, counter_rounds, nullptr, nullptr, 0},
		};
		run_workers(byte_work, 4, go, stop, 4, "SWPB token exchange beside a counter");
		check(byte_work[0].result + byte_work[1].result + bytes[0] == 7 &&
		      (byte_work[0].result | byte_work[1].result | bytes[0]) == 7,
		      "SWPB hands its byte tokens through without loss or duplication");
		check_eq(bytes[1], static_cast<u8>(2 * counter_rounds),
			 "two LDREXB counters on the next byte lose no update beside SWPB");
		check(bytes[2] == 0 && bytes[3] == 0, "and the other bytes of the word stay zero");

		/* Processes: a child on a shared page swaps with LDREX/STREX from
		 * T32 while the parent swaps with SWP. */
		auto *shared_cell = ptr<u32>(shared_region + 512);
		auto *child_result = ptr<volatile u32>(shared_region + 516);
		*shared_cell = 1;
		*child_result = 0;
		const long child = fork_child();
		if (child == 0) {
			*child_result = at_swap_ldrex_thumb(shared_cell, 2, fast_rounds);
			quit(0);
		}
		const u32 parent_token = at_swap_swp_arm(shared_cell, 4, trap_rounds);
		const long status = wait_child(child);
		check(exited_with(status, 0), "the child swapping on the shared page exited");
		const u32 total = *shared_cell + parent_token + *child_result;
		check(total == 7 && (*shared_cell | parent_token | *child_result) == 7,
		      "two processes swapping on one shared page, one by SWP, lose and duplicate no token");
	}

	{
		/* Neighbouring bytes and halfwords of one word, a thread for each. */
		auto *bytes = ptr<u8>(region + 1024);
		bytes[0] = bytes[1] = bytes[2] = bytes[3] = 0;
		Work work[4] = {
			{add_byte, false, bytes + 0, rounds, nullptr, nullptr, 0},
			{add_byte, true, bytes + 1, rounds, nullptr, nullptr, 0},
			{add_byte, false, bytes + 2, rounds, nullptr, nullptr, 0},
			{add_byte, true, bytes + 3, rounds, nullptr, nullptr, 0},
		};
		run_workers(work, 4, go, stop, 4, "LDREXB counters");
		const u8 expect = static_cast<u8>(rounds);
		check(bytes[0] == expect && bytes[1] == expect && bytes[2] == expect && bytes[3] == expect,
		      "four threads of LDREXB/STREXB on the bytes of one word keep their own counts");

		auto *halves = ptr<u16>(region + 1280);
		halves[0] = halves[1] = 0;
		Work half_work[4] = {
			{add_half, false, halves + 0, rounds, nullptr, nullptr, 0},
			{add_half, true, halves + 1, rounds, nullptr, nullptr, 0},
			{add_half, false, halves + 0, rounds, nullptr, nullptr, 0},
			{add_half, true, halves + 1, rounds, nullptr, nullptr, 0},
		};
		run_workers(half_work, 4, go, stop, 4, "LDREXH counters");
		const u16 half_expect = static_cast<u16>(2 * rounds);
		check(halves[0] == half_expect && halves[1] == half_expect,
		      "four threads of LDREXH/STREXH on two halfwords keep their own counts");
	}
	{
		/* 64 bits: carries propagate and no half is lost. */
		auto *counter = ptr<u64>(region + 1536);
		*counter = 0xfffffffffff00000ULL;
		Work work[4] = {
			{add_double, false, counter, rounds, nullptr, nullptr, 0},
			{add_double, true, counter, rounds, nullptr, nullptr, 0},
			{add_cmpxchg64, false, counter, rounds, nullptr, nullptr, 0},
			{add_cmpxchg64, true, counter, rounds, nullptr, nullptr, 0},
		};
		run_workers(work, 4, go, stop, 4, "64-bit counters");
		check_eq64(*counter, 0xfffffffffff00000ULL + 4ULL * rounds,
			   "LDREXD/STREXD and the kuser cmpxchg64 add through a carry into the high word");
	}

	/* A 64-bit pair written by STREXD is never seen torn, by LDREXD or by
	 * an aligned LDRD (single-copy atomic in ARMv8 AArch32). */
	for (int reader = 0; reader < 2; ++reader) {
		auto *pair = ptr<u64>(region + 2048);
		*pair = 0;
		Work work[3] = {
			{pair_writer, false, pair, rounds * 4, nullptr, nullptr, 0},
			{reader ? pair_reader_ldrd : pair_reader_ldrexd, true, pair, 0, nullptr, nullptr, 0},
			{reader ? pair_reader_ldrd : pair_reader_ldrexd, false, pair, 0, nullptr, nullptr, 0},
		};
		run_workers(work, 3, go, stop, 1, "pair writer");
		check_eq(work[1].result + work[2].result, 0,
			 reader ? "an aligned LDRD never reads a torn 64-bit value (A32 and T32)" :
				  "LDREXD never reads a torn 64-bit value (A32 and T32)");
	}

	/* A plain store loop on the same page: the exclusive pair still makes
	 * forward progress when the stores are a reservation granule away (2 KiB
	 * on the same 4 KiB page, and 8 KiB away within one 16 KiB page). */
	{
		static const word distances[] = {2048, 8192};
		for (word distance : distances) {
			auto *counter = ptr<u32>(region + 4096);
			auto *other = ptr<u32>(region + 4096 + distance);
			*counter = 0;
			*other = 0;
			Work work[3] = {
				{add_word, false, counter, rounds, nullptr, nullptr, 0},
				{add_word, true, counter, rounds, nullptr, nullptr, 0},
				{store_loop, false, other, 0, nullptr, nullptr, 0},
			};
			run_workers(work, 3, go, stop, 2, "exclusives beside a store loop");
			check_eq(*counter, 2 * rounds,
				 distance == 2048 ? "LDREX/STREX beside a store loop 2 KiB away on the page makes progress" :
						    "LDREX/STREX beside a store loop 8 KiB away on the page makes progress");
		}
		/* The same with the kernel helper, whose pair is inside the kernel's
		 * page. */
		auto *counter = ptr<u32>(region + 4096);
		auto *other = ptr<u32>(region + 4096 + 2048);
		*counter = 0;
		Work work[3] = {
			{add_cmpxchg, false, counter, rounds, nullptr, nullptr, 0},
			{add_cmpxchg, true, counter, rounds, nullptr, nullptr, 0},
			{store_loop, true, other, 0, nullptr, nullptr, 0},
		};
		run_workers(work, 3, go, stop, 2, "cmpxchg beside a store loop");
		check_eq(*counter, 2 * rounds, "the kuser cmpxchg beside a store loop on the page makes progress");
	}

	/* Two exclusive loops on different words of one cache line and of one
	 * page: both finish and count exactly. */
	{
		static const word offsets[] = {64, 2048, 8192};
		static const char *names[] = {
			"two LDREX/STREX counters 64 bytes apart both finish",
			"two LDREX/STREX counters 2 KiB apart both finish",
			"two LDREX/STREX counters 8 KiB apart both finish",
		};
		for (unsigned index = 0; index < 3; ++index) {
			auto *first = ptr<u32>(region + 20480);
			auto *second = ptr<u32>(region + 20480 + offsets[index]);
			*first = 0;
			*second = 0;
			Work work[4] = {
				{add_word, false, first, rounds, nullptr, nullptr, 0},
				{add_word, true, second, rounds, nullptr, nullptr, 0},
				{add_word, true, first, rounds, nullptr, nullptr, 0},
				{add_word, false, second, rounds, nullptr, nullptr, 0},
			};
			run_workers(work, 4, go, stop, 4, "two counters");
			check(*first == 2 * rounds && *second == 2 * rounds, names[index]);
		}
	}

	/* Processes: a counter in a shared page, from a child and the parent. */
	{
		auto *counter = ptr<u32>(shared_region + 256);
		*counter = 0;
		const long child = fork_child();
		if (child == 0) {
			at_add_ldrex_thumb(counter, rounds);
			at_add_cmpxchg_arm(counter, rounds);
			quit(0);
		}
		at_add_ldrex_arm(counter, rounds);
		at_add_cmpxchg_thumb(counter, rounds);
		const long status = wait_child(child);
		check(exited_with(status, 0), "the child sharing the counter's page exited");
		check_eq(*counter, 4 * rounds, "two processes on one shared page lose no update");
	}

	/* DMB: a data word and its flag, handed back and forth. The consumer
	 * sees the data of the same round, never the one before. */
	{
		auto *slot = ptr<volatile u32>(region + 24576);
		slot[0] = 0;	/* flag */
		slot[16] = 0;	/* data, on another line */
		slot[32] = 0;	/* acknowledgement */
		struct Handoff {
			volatile u32 *slot;
			u32 rounds;
			u32 stale;
		};
		static Handoff handoff;
		handoff = {slot, 20000, 0};
		auto producer = [](void *) -> long {
			for (u32 round = 1; round <= handoff.rounds; ++round) {
				while (handoff.slot[32] != round - 1)
					;
				handoff.slot[16] = round;
				asm volatile("dmb ish" : : : "memory");
				handoff.slot[0] = round;
			}
			return 0;
		};
		auto consumer = [](void *) -> long {
			for (u32 round = 1; round <= handoff.rounds; ++round) {
				while (handoff.slot[0] != round)
					;
				asm volatile("dmb ish" : : : "memory");
				if (handoff.slot[16] != round)
					++handoff.stale;
				asm volatile("dmb ish" : : : "memory");
				handoff.slot[32] = round;
			}
			return 0;
		};
		Thread first, second;
		const bool started = thread_start(first, producer, nullptr) && thread_start(second, consumer, nullptr);
		check(started, "clone starts the two threads of the handoff");
		if (started) {
			const bool a = thread_join(first, deadline_seconds);
			const bool b = thread_join(second, deadline_seconds);
			check(a && b, "the handoff finishes");
			if (!(a && b))
				end_group();
			check_eq(handoff.stale, 0, "DMB orders the data before its flag in the other thread");
		}
	}
}

} // namespace probe
