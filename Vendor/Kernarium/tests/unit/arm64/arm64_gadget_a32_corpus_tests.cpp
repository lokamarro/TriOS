// SPDX-License-Identifier: GPL-2.0-or-later
/* The gadget engine against the AArch32 instruction corpus
 * (tests/unit/arm64/aarch32_corpus/README.md owns the pack). Every case whose
 * forms the engine decodes is run through the production AArch32 loop in the
 * oracle's memory map, with the oracle's exit stubs, and what the pack
 * records is compared: the signal or system call that ended it and the
 * instruction and address it was raised at, every general register, the APSR
 * flags, Q and GE, D0-D31, FPSCR, TPIDRURW, the data region, and the retired
 * count (the pack counts the program's instructions; the engine also retires
 * the exit stub's load and, for a system call or a breakpoint, that
 * instruction).
 *
 * A case whose forms need an extension the engine's host lacks is skipped;
 * the engine raises the illegal-instruction exit for them, which the forms
 * test checks. The `unaligned` and
 * `fpscr-reserved` suites are recorded from hardware (a Raspberry Pi 4): a
 * multi-word access at a misaligned address is an alignment fault with
 * SCTLR.A = 0, and Stride and Len read as zero.
 *
 * A store-exclusive may fail without cause, as the architecture allows. The
 * gadgets run the host's own exclusives, and the host's monitor ends at any
 * interrupt or preemption between the load and the store, so under load one
 * does land in that window now and then. The oracle's store-exclusive never
 * fails and a guest loops, but a corpus case is one attempt: a case whose
 * only difference from the pack is that failed store-exclusive runs again,
 * up to kExclusiveAttempts times in all, and must then agree completely.
 * --interrupt interrupts the thread that runs the guest with a timer signal
 * every few microseconds so that this happens on purpose. Whether one lands
 * in a window of tens of nanoseconds is chance, so the passes go on past
 * --passes until a store-exclusive has lost its reservation, for at most
 * kInterruptDeadline, and the run fails if none did. --passes N repeats the
 * selected records at least N times.
 *
 *   uml_arm64_gadget_a32_corpus_tests PACK [--record NAME[,NAME...]]
 *       [--case TEXT] [--limit N] [--forms FILE] [--passes N] [--interrupt]
 */
#include "aarch32_corpus_cpu_state.hpp"
#include "aarch32_corpus_pack.hpp"
#include "arm64_gadget_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/semantics_exit_counters.hpp"
#include "cpu/arm64/aarch32/state_format.hpp"
#include "scratch_backing.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <sys/time.h>

namespace {

namespace corpus = ::uml::testing::aarch32_corpus;
namespace decode = ::uml::cpu::arm64::aarch32;
using namespace ::uml::emulator::tests::a32;
using ::uml::testing::ScratchBacking;

constexpr std::uint32_t svc_word = 0xef00'0000U;
constexpr std::size_t budget = 20'000;
/* How many times a case whose store-exclusive lost its reservation may run.
 * A lost reservation is an interrupt that landed in a window of tens of
 * nanoseconds, so a few attempts are enough; an engine whose store-exclusive
 * never succeeds fails every one. */
constexpr unsigned kExclusiveAttempts = 16;
/* How long --interrupt may go on looking for a lost reservation. A pass takes
 * a fraction of a second and loses one within a few, even with two dozen
 * such processes sharing ten cores, so reaching this means the host keeps its
 * monitor across an interrupt. */
constexpr std::chrono::seconds kInterruptDeadline{60};

/* The oracle's memory map and exit stubs. */
class Machine final {
public:
	Machine()
		: generation_{3000}
		, code_{"a32corpus-code.XXXXXX", corpus::kCodeSize}
		, data_{"a32corpus-data.XXXXXX", corpus::kDataSize}
		, read_only_{"a32corpus-ro.XXXXXX", corpus::kReadOnlySize}
		, executor_{::uml::cpu::arm64::gadgets::BuildOptions{}}
	{
		require(darwin_uml_static_vm_activate(generation_) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(std::uintptr_t{corpus::kCodeBase}),
			code_.descriptor(), 0, corpus::kCodeSize,
			PROT_READ | PROT_WRITE | PROT_EXEC, 1, 1) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(std::uintptr_t{corpus::kDataBase}),
			data_.descriptor(), 0, corpus::kDataSize,
			PROT_READ | PROT_WRITE, 0, 1) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(std::uintptr_t{corpus::kReadOnlyBase}),
			read_only_.descriptor(), 0, corpus::kReadOnlySize, PROT_READ,
			0, 0) == 0);
		const auto fill = corpus::read_only_fill();
		read_only_.put(0, std::as_bytes(std::span{fill}));
	}

	~Machine() { require(darwin_uml_static_vm_destroy(generation_) == 0); }

	Machine(const Machine &) = delete;
	Machine &operator=(const Machine &) = delete;

	[[nodiscard]] unsigned long generation() const noexcept
	{
		return generation_;
	}

	/* Code as the oracle prepares it, and this engine's exit code, a
	 * supervisor call, behind the two epilogues; returns whether a NOP
	 * precedes the exit stub that ends the program. */
	[[nodiscard]] bool install(const corpus::Case &test)
	{
		bool padded = false;
		copy(corpus::kCodeBase, corpus::code_image(test, svc_word, padded));
		return padded;
	}

	/* Written through the guest mapping, so the host pages are already
	 * writable when an exclusive store reaches them (the first store to a
	 * shared page takes a fault that clears the host monitor, and the
	 * store-exclusive may then fail, as the architecture allows). */
	/* The static MMU copies one guest page at a time. */
	static void copy(std::uint32_t address, const std::vector<std::uint8_t> &bytes)
	{
		for (std::size_t offset = 0; offset < bytes.size(); offset += page)
			require(darwin_uml_static_vm_copy_from_buffer(address + offset,
				bytes.data() + offset,
				std::min<std::size_t>(page, bytes.size() - offset)) == 0);
	}

	void set_data(const std::vector<std::uint8_t> &bytes)
	{
		copy(corpus::kDataBase, bytes);
	}

	[[nodiscard]] std::vector<std::uint8_t> data() const
	{
		std::vector<std::uint8_t> bytes(corpus::kDataSize);
		data_.get(0, std::as_writable_bytes(std::span{bytes}));
		return bytes;
	}

	[[nodiscard]] ExecutionResult run(::uml::cpu::arm64::CpuState &cpu)
	{
		::uml::cpu::ExecutionRequest request{
			.cpu = cpu,
			.memory = memory_,
			.translated_memory = &memory_,
			.generation = generation_,
			.instruction_budget = budget,
		};
		/* The low halves of v8 to v15 are callee-saved in the host's calling
		 * convention, and no gadget may write them. */
		const std::uint64_t before[8] = {
			0x0101'0101'0101'0101ULL, 0x0202'0202'0202'0202ULL,
			0x0303'0303'0303'0303ULL, 0x0404'0404'0404'0404ULL,
			0x0505'0505'0505'0505ULL, 0x0606'0606'0606'0606ULL,
			0x0707'0707'0707'0707ULL, 0x0808'0808'0808'0808ULL};
		std::uint64_t after[8] = {};
		asm volatile("ldp d8, d9, [%0]\n\tldp d10, d11, [%0, #16]\n\t"
			     "ldp d12, d13, [%0, #32]\n\tldp d14, d15, [%0, #48]"
			     :: "r"(before) : "memory", "v8", "v9", "v10", "v11",
			     "v12", "v13", "v14", "v15");
		auto result = ::uml::cpu::arm64::detail::run_translated_aarch32_blocks(
			executor_, request);
		asm volatile("stp d8, d9, [%0]\n\tstp d10, d11, [%0, #16]\n\t"
			     "stp d12, d13, [%0, #32]\n\tstp d14, d15, [%0, #48]"
			     :: "r"(after) : "memory");
		for (unsigned index = 0; index < 8; ++index)
			require(after[index] == before[index],
				"a gadget wrote a callee-saved vector register");
		return result;
	}

	/* The word at a code address, for the exits Linux turns into SIGTRAP. */
	[[nodiscard]] std::uint32_t code_word(std::uint32_t address)
	{
		std::uint32_t word = 0;
		(void)memory_.fetch(address,
			std::as_writable_bytes(std::span{&word, 1}));
		return word;
	}

private:
	unsigned long generation_;
	ScratchBacking code_;
	ScratchBacking data_;
	ScratchBacking read_only_;
	StaticGuestMemory memory_;
	::uml::cpu::arm64::gadgets::GadgetExecutor executor_;
};

/* The decoder's view of the program, to skip a case that needs an extension
 * the engine does not give its guest: the engine decodes against the
 * extensions its host has (a32::decode_profile), so a word the whole profile
 * accepts and the engine's profile rejects needs one it lacks. On a host
 * without BF16, I8MM or JSCVT, say, the corpus's cases of those forms
 * (recorded on an oracle that has them) are skipped. */
[[nodiscard]] bool needs_absent_extension(const corpus::Case &test)
{
	const auto engine = ::uml::cpu::arm64::gadgets::a32::decode_profile(
		::uml::cpu::arm64::gadgets::BuildOptions{});
	bool absent = false;
	corpus::for_each_instruction(
		test, decode::Profile{},
		[&](const corpus::ProgramStep &step) {
			absent = step.instruction.legal() &&
				 !step.decode_at(engine).legal();
			return !absent;
		});
	return absent;
}

[[nodiscard]] bool linux_breakpoint(std::uint32_t word, bool thumb)
{
	/* arm64's compat layer, as arm's, raises SIGTRAP for these. */
	/* `word` is the guest's little-endian word at the PC: a T32
	 * instruction's first halfword is the low half. */
	return thumb ? (word & 0xffffU) == 0xde01U || word == 0xa000'f7f0U
		     : word == 0xe7f0'01f0U;
}

struct Outcome final {
	unsigned event{};
	bool exit_thumb{};
	std::uint32_t half{};
	/* Whole: an AArch32 fault address is a 32-bit one, and wider is the
	 * engine's error, not the guest's. */
	std::uint64_t address{};
	std::size_t retired{};
	bool valid{true};
};

[[nodiscard]] Outcome classify(Machine &machine,
			       const ::uml::cpu::arm64::CpuState &cpu,
			       const ExecutionResult &result,
			       std::uint32_t entry, std::size_t halves)
{
	Outcome outcome;
	outcome.retired = result.retired_instructions;
	const auto pc = decode::key_pc(cpu.pc);
	const bool thumb = decode::key_is_thumb(cpu.pc);
	/* The pack numbers halfwords from the start of the program, and
	 * records 0 for a PC outside it: the fetch fault of a branch to an
	 * unfetchable target or of a program that falls off the region. */
	const auto half_of = [entry, halves](std::uint32_t address) {
		return address >= entry && (address - entry) / 2U < halves ?
			(address - entry) / 2U : 0U;
	};
	switch (result.reason) {
	case ExitReason::system_call:
		if (pc == corpus::kArmEpilogue + 4U) {
			outcome.event = 0;
		} else if (pc == corpus::kThumbEpilogue + 4U) {
			outcome.event = 0;
			outcome.exit_thumb = true;
		} else {
			outcome.event = corpus::kEventSvc;
			outcome.half = half_of(pc - (thumb ? 2U : 4U));
		}
		break;
	case ExitReason::memory_fault:
		outcome.event = result.fault.alignment ? corpus::kSigBus :
							 corpus::kSigSegv;
		outcome.half = half_of(pc);
		outcome.address = result.fault.address;
		break;
	case ExitReason::illegal_instruction:
		outcome.half = half_of(pc);
		outcome.address = pc;
		outcome.event = linux_breakpoint(machine.code_word(pc), thumb) ?
			corpus::kSigTrap : corpus::kSigIll;
		break;
	case ExitReason::breakpoint:
		outcome.event = corpus::kSigTrap;
		outcome.half = half_of(pc);
		outcome.address = pc;
		break;
	case ExitReason::pc_alignment:
		/* The fault is the target's; the branch that made it is the last
		 * instruction retired, which the engine does not name. */
		outcome.event = corpus::kSigBus;
		outcome.half = ~0U;
		outcome.address = result.fault.address;
		break;
	case ExitReason::instruction_budget:
		outcome.event = 14;
		break;
	default:
		outcome.valid = false;
		break;
	}
	return outcome;
}

/* The task's saved vectors: CpuState holds only a pointer to them until a
 * program that touches them loads them, as the runtime enters a task. */
using SavedVectors = decltype(::uml::cpu::arm64::CpuState::vectors);

void load_state(::uml::cpu::arm64::CpuState &cpu, SavedVectors &saved,
		const corpus::State &state, bool thumb, std::uint32_t entry)
{
	for (unsigned index = 0; index < corpus::kRegisters; ++index)
		cpu.registers[index] = state.r[index];
	cpu.nzcv = state.apsr & 0xf000'0000U;
	cpu.a32_q = static_cast<std::uint8_t>(state.apsr >> 27 & 1U);
	std::uint32_t mask = 0;
	for (unsigned lane = 0; lane < 4; ++lane)
		if (state.apsr >> (16 + lane) & 1U)
			mask |= 0xffU << (8 * lane);
	cpu.a32_ge_mask = mask;
	cpu.tls = state.tpidrurw;
	cpu.a32_tpidruro = state.tpidruro;
	for (unsigned index = 0; index < corpus::kVectors; ++index)
		std::memcpy(saved[index / 2].bytes.data() + 8 * (index % 2),
			    &state.d[index], 8);
	cpu.defer_fp_load(saved.data(), state.fpscr & decode::fpscr_control_mask,
			  state.fpscr & ::uml::cpu::arm64::fpsr_writable_mask,
			  ::uml::cpu::arm64::fpcr_writable_mask);
	cpu.a32_fpscr_nzcv = state.fpscr & decode::fpscr_nzcv;
	cpu.a32_cp15ben = 1;
	cpu.a32 = 1;
	cpu.pc = thumb ? decode::t32_key(entry) : decode::a32_key(entry);
}

struct Totals final {
	std::size_t run = 0, skipped = 0, failed = 0, printed = 0;
	/* Cases that agreed with the pack only on a later attempt, after a
	 * store-exclusive lost its reservation. */
	std::size_t retried = 0;
	/* Attempts that lost a reservation, whatever became of their case. */
	std::size_t lost = 0;
	/* The encodings under test of the cases that ran, for --forms. */
	std::set<std::string> forms;
};

struct Options final {
	std::string record, text;
	std::size_t limit = 40;
	std::size_t passes = 1;
	bool explain = false;
	/* Interrupt the guest's thread continuously, and go on until a
	 * store-exclusive has lost its reservation to it. */
	bool interrupt = false;
	/* Write the encodings whose cases ran, one per line, to this file. */
	std::string forms_path;

	/* Whether --record names the record: one name, or several separated
	 * by commas; none selects every record. */
	[[nodiscard]] bool selects(std::string_view name) const
	{
		if (record.empty())
			return true;
		std::string_view rest = record;
		while (!rest.empty()) {
			const auto comma = rest.find(',');
			if (rest.substr(0, comma) == name)
				return true;
			if (comma == std::string_view::npos)
				break;
			rest.remove_prefix(comma + 1);
		}
		return false;
	}
};

/* Host interrupts on demand: an interval timer sends SIGALRM to the process
 * every few microseconds, and the process has one thread, the one that runs
 * the guest. Each delivery enters and leaves the kernel on the interrupted
 * thread, which ends the host's exclusive monitor as a timer tick, an
 * inter-processor interrupt or a preemption does on a loaded host. The timer
 * is the kernel's, so no thread of ours has to be scheduled for it to fire,
 * as one that sent the signals would. The handler does nothing and stays
 * installed after the timer stops, so that a signal already pending when it
 * does is not fatal. */
class InterruptStorm final {
public:
	InterruptStorm()
	{
		struct sigaction action {};
		action.sa_handler = [](int) {};
		sigemptyset(&action.sa_mask);
		action.sa_flags = SA_RESTART;
		require(::sigaction(SIGALRM, &action, nullptr) == 0);
		struct itimerval timer {};
		timer.it_interval.tv_usec = timer.it_value.tv_usec = kInterval;
		require(::setitimer(ITIMER_REAL, &timer, nullptr) == 0);
	}

	~InterruptStorm()
	{
		struct itimerval off {};
		(void)::setitimer(ITIMER_REAL, &off, nullptr);
	}

	InterruptStorm(const InterruptStorm &) = delete;
	InterruptStorm &operator=(const InterruptStorm &) = delete;

private:
	/* Microseconds. Shorter fills the host with timer interrupts for no
	 * more losses; longer loses fewer. */
	static constexpr suseconds_t kInterval = 5;
};

/* The program's words and what the decoder says of the instruction under
 * test. */
void explain(const corpus::Case &test, std::size_t target)
{
	std::fprintf(stderr, "    program %s:", test.flags & corpus::kFlagThumb ? "T32" : "A32");
	for (const auto half : test.halves)
		std::fprintf(stderr, " %04x", half);
	std::fprintf(stderr, "  target #%zu\n", target);
	std::fprintf(stderr, "    initial fpscr %#x; final fpscr %#x\n",
		     test.initial.fpscr, test.final.fpscr);
	for (unsigned index = 0; index < corpus::kVectors; index += 4)
		std::fprintf(stderr,
			     "    d%-2u %016llx %016llx %016llx %016llx\n", index,
			     static_cast<unsigned long long>(test.initial.d[index]),
			     static_cast<unsigned long long>(test.initial.d[index + 1]),
			     static_cast<unsigned long long>(test.initial.d[index + 2]),
			     static_cast<unsigned long long>(test.initial.d[index + 3]));
	const bool thumb = (test.flags & corpus::kFlagThumb) != 0;
	decode::ItState it;
	std::size_t index = 0;
	for (std::size_t half = 0; half < test.halves.size() && index <= target;) {
		std::uint32_t word;
		unsigned length;
		if (!thumb) {
			if (half + 1 >= test.halves.size())
				return;
			word = std::uint32_t{test.halves[half + 1]} << 16 | test.halves[half];
			length = 4;
		} else {
			length = decode::thumb_length(test.halves[half]);
			if (half + length / 2 > test.halves.size())
				return;
			word = length == 4 ? std::uint32_t{test.halves[half]} << 16 | test.halves[half + 1]
					   : std::uint32_t{test.halves[half]} << 16;
		}
		const decode::Profile profile;
		const auto instruction = thumb ? decode::decode_thumb(word, it, profile)
					       : decode::decode_a32(word, profile);
		if (index == target)
			std::fprintf(stderr, "    %s status %d reason %d cond %u\n",
				     instruction.info().name,
				     static_cast<int>(instruction.status),
				     static_cast<int>(instruction.reason), instruction.cond);
		it = instruction.next_it;
		half += length / 2;
		++index;
	}
}

/* What one run of a case found. */
struct Attempt final {
	std::string diffs;
	/* The only differences are a store-exclusive that reported failure where
	 * the oracle's succeeded and the store it therefore did not make. */
	bool reservation_lost = false;
};

[[nodiscard]] bool is_store_exclusive(const corpus::Case &test)
{
	return test.form.starts_with("STREX") || test.form.starts_with("STLEX");
}

[[nodiscard]] Attempt run_case(Machine &machine, const corpus::Case &test,
			       const std::vector<std::uint8_t> &initial_data,
			       const std::vector<std::uint8_t> &expected_data)
{
	const bool thumb = (test.flags & corpus::kFlagThumb) != 0;
	const std::uint32_t origin = test.origin;
	const bool nop_before_stub = machine.install(test);
	machine.set_data(initial_data);
	::uml::cpu::arm64::CpuState cpu{};
	corpus::leave_aarch64_task_state(cpu);
	SavedVectors saved{};
	load_state(cpu, saved, test.initial, thumb, corpus::kCodeBase + origin);
	const auto result = machine.run(cpu);
	/* What the task's context switch saves: the vectors a program wrote,
	 * which only a program marked as touching them can have. */
	if (cpu.fp_dirty)
		saved = cpu.vectors;
	const auto outcome = classify(machine, cpu, result,
				      corpus::kCodeBase + origin,
				      test.halves.size());

	Attempt attempt;
	auto &diffs = attempt.diffs;
	std::size_t notes = 0, lost_status = 0, unwritten = 0;
	const auto note = [&](const char *format, auto... values) {
		char text[200];
		std::snprintf(text, sizeof(text), format, values...);
		diffs += diffs.empty() ? "" : "; ";
		diffs += text;
		++notes;
	};
	if (!outcome.valid) {
		note("unexpected exit %d", static_cast<int>(result.reason));
	} else if (test.event == 14 || outcome.event == 14) {
		/* A loop neither side leaves: where it stops is not comparable. */
		if (test.event != outcome.event)
			note("event %u vs %u", test.event, outcome.event);
		if (diffs.empty())
			return attempt;
	} else {
		if (test.event != outcome.event) {
			note("event %u vs %u", test.event, outcome.event);
		} else if (test.event == 0) {
			if (((test.flags & corpus::kFlagExitThumb) != 0) != outcome.exit_thumb)
				note("exit state %d vs %d",
				     (test.flags & corpus::kFlagExitThumb) != 0,
				     outcome.exit_thumb);
		} else {
			if (outcome.half != ~0U && test.fault_instruction != outcome.half)
				note("fault at halfword %u vs %u", test.fault_instruction,
				     outcome.half);
			if ((test.event == corpus::kSigSegv || test.event == corpus::kSigBus ||
			     test.event == corpus::kSigIll) &&
			    test.fault_address != outcome.address)
				note("fault address %#x vs %#llx", test.fault_address,
				     static_cast<unsigned long long>(outcome.address));
		}
		/* The pack counts the program's instructions: the engine adds the
		 * exit stub's load and the exit's system call, and a system call
		 * or a breakpoint itself. */
		std::size_t expected = test.retired;
		/* A Thumb program that leaves the stub's word unaligned has a NOP
		 * before it, executed only when the program falls off its end. */
		const bool padded = nop_before_stub && test.event == 0;
		if (test.event == 0) {
			expected += 2;
		} else if (test.event == corpus::kEventSvc ||
			   result.reason == ExitReason::breakpoint) {
			expected += 1;
		}
		if (outcome.retired != expected && !(padded && outcome.retired == expected + 1))
			note("retired %zu vs %zu", test.retired, outcome.retired);
		for (unsigned index = 0; index < corpus::kRegisters; ++index) {
			const auto value = static_cast<std::uint32_t>(cpu.registers[index]);
			/* A register's slot stays zero-extended: the shared A64
			 * gadgets read it whole. */
			if ((cpu.registers[index] >> 32) != 0)
				note("r%u's slot %#llx is not zero-extended", index,
				     static_cast<unsigned long long>(cpu.registers[index]));
			if (value != test.final.r[index]) {
				note("r%u %#x vs %#x", index, test.final.r[index], value);
				/* A store-exclusive's status: 0 stored, 1 did not. */
				if (test.final.r[index] == 0 && value == 1)
					++lost_status;
			}
		}
		const auto apsr = corpus::apsr_of(cpu);
		if (apsr != (test.final.apsr & 0xf80f'0000U))
			note("apsr %#x vs %#x", test.final.apsr & 0xf80f'0000U, apsr);
		for (unsigned index = 0; index < corpus::kVectors; ++index) {
			std::uint64_t value;
			std::memcpy(&value, saved[index / 2].bytes.data() +
					    8 * (index % 2), 8);
			if (value != test.final.d[index])
				note("d%u %#llx vs %#llx", index,
				     static_cast<unsigned long long>(test.final.d[index]),
				     static_cast<unsigned long long>(value));
		}
		const std::uint32_t fpscr = (cpu.effective_fpcr() & decode::fpscr_control_mask) |
			(cpu.effective_fpsr() & ::uml::cpu::arm64::fpsr_writable_mask) |
			(cpu.a32_fpscr_nzcv & decode::fpscr_nzcv);
		const std::uint32_t fpscr_mask = decode::fpscr_control_mask |
			::uml::cpu::arm64::fpsr_writable_mask | decode::fpscr_nzcv;
		if (fpscr != (test.final.fpscr & fpscr_mask))
			note("fpscr %#x vs %#x", test.final.fpscr & fpscr_mask, fpscr);
		if (static_cast<std::uint32_t>(cpu.tls) != test.final.tpidrurw)
			note("tpidrurw %#x vs %#x", test.final.tpidrurw,
			     static_cast<std::uint32_t>(cpu.tls));
		if (machine.data() != expected_data) {
			const auto actual = machine.data();
			std::size_t count = 0, first = 0;
			for (std::size_t index = 0; index < actual.size(); ++index)
				if (actual[index] != expected_data[index]) {
					if (count++ == 0)
						first = index;
				}
			note("data differs in %zu bytes, first at %#zx", count, first);
			unwritten = actual == initial_data ? 1 : 0;
		}
	}
	attempt.reservation_lost = is_store_exclusive(test) && lost_status == 1 &&
		unwritten == 1 && notes == 2;
	return attempt;
}

void check_case(Machine &machine, const corpus::Record &record,
		const corpus::Case &test, const Options &options, Totals &totals)
{
	if (needs_absent_extension(test)) {
		++totals.skipped;
		return;
	}
	const auto initial_data = corpus::data_before(test);
	const auto expected_data = corpus::data_after(test, initial_data);
	++totals.run;
	totals.forms.insert(test.form);

	auto attempt = run_case(machine, test, initial_data, expected_data);
	unsigned attempts = 1;
	totals.lost += attempt.reservation_lost ? 1 : 0;
	while (attempt.reservation_lost && attempts < kExclusiveAttempts) {
		attempt = run_case(machine, test, initial_data, expected_data);
		totals.lost += attempt.reservation_lost ? 1 : 0;
		++attempts;
	}
	if (attempt.diffs.empty()) {
		if (attempts > 1)
			++totals.retried;
		return;
	}
	++totals.failed;
	if (totals.printed++ < options.limit) {
		std::fprintf(stderr, "%s: %s [%s]: %s", record.name.c_str(),
			     test.name.c_str(), test.form.c_str(),
			     attempt.diffs.c_str());
		if (attempts > 1)
			std::fprintf(stderr, " (%u attempts)", attempts);
		std::fprintf(stderr, "\n");
		if (options.explain)
			explain(test, test.target);
	}
}

} // namespace

int main(int argc, char **argv)
{
	const ::uml::testing::RuntimeTestRoute runtime_route;
	const char *path = nullptr;
	Options options;
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--record" && index + 1 < argc) {
			options.record = argv[++index];
		} else if (argument == "--case" && index + 1 < argc) {
			options.text = argv[++index];
		} else if (argument == "--forms" && index + 1 < argc) {
			options.forms_path = argv[++index];
		} else if (argument == "--passes" && index + 1 < argc) {
			options.passes = static_cast<std::size_t>(std::atoi(argv[++index]));
		} else if (argument == "--interrupt") {
			options.interrupt = true;
		} else if (argument == "--explain") {
			options.explain = true;
		} else if (argument == "--limit" && index + 1 < argc) {
			options.limit = static_cast<std::size_t>(std::atoi(argv[++index]));
		} else {
			require(path == nullptr && !argument.starts_with("--"),
				"usage: PACK [--record NAME] [--case TEXT]");
			path = argv[index];
		}
	}
	require(path != nullptr, "usage: PACK [--record NAME]");
	const auto pack = corpus::read_pack(corpus::read_file(path));
	Machine machine;
	Totals totals;
	decode::reset_a32_library_exits();
	decode::count_a32_library_exits(true);
	std::optional<InterruptStorm> storm;
	if (options.interrupt)
		storm.emplace();
	const auto deadline = std::chrono::steady_clock::now() + kInterruptDeadline;
	for (std::size_t pass = 1;; ++pass) {
		/* The lines of this pass; the last one's are printed. */
		std::vector<std::string> lines;
		for (const auto &record : pack.records) {
			if ((record.profiles & corpus::kProfileBit) == 0 ||
			    !options.selects(record.name))
				continue;
			Totals before = totals;
			for (const auto &test : record.cases) {
				if (!options.text.empty() &&
				    test.name.find(options.text) == std::string::npos)
					continue;
				check_case(machine, record, test, options, totals);
			}
			char line[160];
			std::snprintf(line, sizeof(line),
				      "  %-24s run %5zu  skipped %5zu  failed %5zu\n",
				      record.name.c_str(), totals.run - before.run,
				      totals.skipped - before.skipped,
				      totals.failed - before.failed);
			lines.emplace_back(line);
		}
		const bool enough = pass >= options.passes &&
			(!options.interrupt || totals.lost != 0);
		if (enough || (options.interrupt &&
			       std::chrono::steady_clock::now() >= deadline)) {
			for (const auto &line : lines)
				std::fputs(line.c_str(), stdout);
			break;
		}
	}
	if (!options.forms_path.empty()) {
		std::FILE *out = std::fopen(options.forms_path.c_str(), "w");
		require(out != nullptr, "cannot write the form list");
		for (const auto &form : totals.forms)
			std::fprintf(out, "%s\n", form.c_str());
		std::fclose(out);
	}
	storm.reset();
	std::printf("arm64-gadget-a32-corpus: %zu cases run, %zu skipped above the "
		    "level or needing an extension the host lacks, %zu failed "
		    "(%zu exclusive retried), %llu in the library\n",
		    totals.run, totals.skipped, totals.failed, totals.retried,
		    static_cast<unsigned long long>(
			    ::uml::emulator::tests::a32::check_library_exits()));
	/* A storm that never ended a reservation proved nothing. */
	if (options.interrupt && totals.lost == 0) {
		std::fprintf(stderr, "--interrupt: no store-exclusive lost its "
			     "reservation, so none was interrupted\n");
		return 1;
	}
	return totals.failed == 0 ? 0 : 1;
}
