// SPDX-License-Identifier: GPL-2.0-or-later
#include "program.hpp"
#include "exception_checks.hpp"
#include "executor.hpp"
#include "native_builder.hpp"
#include "native_entry.hpp"

#include "cpu/x86_64/segments.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace uml::cpu::x86_64::gadgets {

static_assert(std::is_trivially_copyable_v<Gadget>,
	"a replayed block copies its decoded instructions into its own storage");
static_assert(alignof(Gadget) <= alignof(std::max_align_t),
	"the retained instructions are allocated as bytes");

namespace {

[[nodiscard]] bool fatal_trace_enabled() noexcept
{
	const auto *value = std::getenv("UML_X86_FATAL_TRACE");
	return value != nullptr && value[0] != '\0' && value[0] != '0';
}

void trace_fatal(const char *stage, const Program &program,
	const CpuState &cpu, std::size_t instruction_budget,
	std::size_t executed = 0) noexcept
{
	if (!fatal_trace_enabled())
		return;
	std::fprintf(stderr,
		"Darwin x86 gadget fatal stage=%s cpu-rip=%#llx program-rip=%#llx "
		"cpu-mode=%u program-mode=%u native-words=%zu instructions=%zu "
		"budget=%zu executed=%zu\n",
		stage,
		static_cast<unsigned long long>(cpu.rip),
		static_cast<unsigned long long>(program.address()),
		static_cast<unsigned>(cpu.execution_mode),
		static_cast<unsigned>(program.execution_mode()),
		program.native_stream().size(), program.instruction_count(),
		instruction_budget, executed);
	std::fflush(stderr);
}

/* The decoded instructions and the native words a translation assembles are
 * built into buffers the thread keeps, so the storage a build works in is
 * bought once per thread rather than once per block. Only the finished block
 * is allocated, at its own size. */
[[nodiscard]] std::vector<Gadget> &decoded_scratch() noexcept
{
	thread_local std::vector<Gadget> scratch;
	return scratch;
}

[[nodiscard]] std::vector<std::uintptr_t> &native_scratch() noexcept
{
	thread_local std::vector<std::uintptr_t> scratch;
	return scratch;
}

[[nodiscard]] std::vector<std::byte> &source_scratch() noexcept
{
	thread_local std::vector<std::byte> scratch;
	return scratch;
}

/* Where a replay's decode lands before the program keeps it. A build is
 * finished before any of this runs, so the replay has a buffer of its own
 * rather than sharing the build's. */
[[nodiscard]] std::vector<Gadget> &replay_scratch() noexcept
{
	thread_local std::vector<Gadget> scratch;
	return scratch;
}

/* Decode a program's instructions back out of the bytes its own build read.
 * Those bytes are the block, so this reproduces exactly the instructions the
 * block was built from -- deterministically, and without reading guest memory,
 * which has been free to change since and whose newer contents are no business
 * of a block already entered. The block's instructions end where its source
 * does; anything else means the two do not describe the same block.
 */
[[nodiscard]] bool decode_block(const Program &program,
	std::vector<Gadget> &gadgets) noexcept
{
	try {
		const auto source = program.source();
		const auto mode = program.execution_mode();
		gadgets.clear();
		gadgets.reserve(program.instruction_count());
		auto current = program.address();
		std::size_t offset = 0;
		for (std::size_t index = 0; index < program.instruction_count();
			++index) {
			if (offset >= source.size())
				break;
			const auto decoded = decode(
				source.subspan(offset), current, mode);
			if (decoded.status != DecodeStatus::decoded)
				break;
			gadgets.push_back({current, decoded.instruction});
			offset += decoded.instruction.length;
			current = advance_instruction_pointer(
				mode, current, decoded.instruction.length);
		}
		if (gadgets.size() == program.instruction_count() &&
			offset == source.size())
			return true;
		gadgets.clear();
		return false;
	} catch (...) {
		gadgets.clear();
		return false;
	}
}

} // namespace

std::span<const Gadget> Program::replay_instructions() const noexcept
{
	const auto kept = [this](const std::byte *decoded) {
		return std::span<const Gadget>{
			reinterpret_cast<const Gadget *>(decoded), instructions_};
	};
	if (const auto *decoded = decoded_.load(std::memory_order_acquire);
	    decoded != nullptr)
		return kept(decoded);
	auto &gadgets = replay_scratch();
	if (!decode_block(*this, gadgets))
		return {};
	const auto bytes = gadgets.size() * sizeof(Gadget);
	try {
		auto storage = std::make_unique_for_overwrite<std::byte[]>(bytes);
		std::memcpy(storage.get(), gadgets.data(), bytes);
		std::byte *expected = nullptr;
		if (decoded_.compare_exchange_strong(expected, storage.get(),
			    std::memory_order_acq_rel, std::memory_order_acquire))
			return kept(storage.release());
		/* Another executor of a shared program published first. */
		return kept(expected);
	} catch (...) {
		/* Without the storage this decode still stands for this one
		 * replay; the next one decodes again. */
		return {gadgets.data(), gadgets.size()};
	}
}

Program::Program(ExecutionMode mode, std::uint64_t address,
	std::span<const Gadget> gadgets, std::span<const std::byte> source,
	std::span<const std::uintptr_t> words, bool check_head)
	: address_{normalize_instruction_pointer(mode, address)},
	  mode_{mode}, check_head_{check_head}, owns_{true}
{
	auto stream = std::make_unique_for_overwrite<std::uintptr_t[]>(
		stream_header_words + words.size());
	auto bytes = std::make_unique_for_overwrite<std::byte[]>(source.size());
	stream[0] = address_;
	stream[1] = address_ + source.size();
	if (!words.empty())
		std::memcpy(stream.get() + stream_header_words, words.data(),
			words.size_bytes());
	if (!source.empty())
		std::memcpy(bytes.get(), source.data(), source.size());
	words_ = stream.release();
	source_bytes_ = bytes.release();
	words_count_ = static_cast<std::uint32_t>(words.size());
	instructions_ = static_cast<std::uint32_t>(gadgets.size());
	source_ = static_cast<std::uint32_t>(source.size());
	terminal_ = !gadgets.empty() && gadgets.back().instruction.terminal();
}

Program::~Program()
{
	release();
}

Program::Program(Program &&other) noexcept
	: address_{other.address_}, words_{other.words_},
	  source_bytes_{other.source_bytes_},
	  decoded_{other.decoded_.exchange(nullptr, std::memory_order_acq_rel)},
	  words_count_{other.words_count_}, instructions_{other.instructions_},
	  source_{other.source_}, mode_{other.mode_},
	  terminal_{other.terminal_}, check_head_{other.check_head_},
	  owns_{other.owns_}
{
	other.words_ = nullptr;
	other.source_bytes_ = nullptr;
	other.words_count_ = 0;
	other.instructions_ = 0;
	other.source_ = 0;
	other.owns_ = false;
}

Program &Program::operator=(Program &&other) noexcept
{
	if (this == &other)
		return *this;
	release();
	address_ = other.address_;
	words_ = other.words_;
	source_bytes_ = other.source_bytes_;
	decoded_.store(other.decoded_.exchange(nullptr,
		std::memory_order_acq_rel), std::memory_order_release);
	words_count_ = other.words_count_;
	instructions_ = other.instructions_;
	source_ = other.source_;
	mode_ = other.mode_;
	terminal_ = other.terminal_;
	check_head_ = other.check_head_;
	owns_ = other.owns_;
	other.words_ = nullptr;
	other.source_bytes_ = nullptr;
	other.words_count_ = 0;
	other.instructions_ = 0;
	other.source_ = 0;
	other.owns_ = false;
	return *this;
}

void Program::release() noexcept
{
	if (owns_) {
		delete[] words_;
		delete[] source_bytes_;
	}
	delete[] decoded_.exchange(nullptr, std::memory_order_acq_rel);
	words_ = nullptr;
	source_bytes_ = nullptr;
	owns_ = false;
}

Program Program::view_in(std::uintptr_t *stream, std::byte *source) const noexcept
{
	assert(stream != nullptr && (source != nullptr || source_ == 0));
	std::memcpy(stream, words_, stream_bytes(words_count_));
	if (source_ != 0)
		std::memcpy(source, source_bytes_, source_);
	Program view;
	view.address_ = address_;
	view.words_ = stream;
	view.source_bytes_ = source;
	view.words_count_ = words_count_;
	view.instructions_ = instructions_;
	view.source_ = source_;
	view.mode_ = mode_;
	view.terminal_ = terminal_;
	view.check_head_ = check_head_;
	view.owns_ = false;
	return view;
}

BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, std::size_t maximum_instructions) noexcept

{
	return build(memory, address, ExecutionMode::long64,
		maximum_instructions);
}

BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, ExecutionMode mode,
	std::size_t maximum_instructions) noexcept
{
	return build(memory, address, mode, maximum_instructions, {});
}

BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, ExecutionMode mode,
	std::size_t maximum_instructions, ProgramLinkResolver links) noexcept
{
	return build(memory, address, mode, maximum_instructions, links, 0);
}

BuildResult build(::uml::memory::GuestAddressSpace &memory,
	std::uint64_t address, ExecutionMode mode,
	std::size_t maximum_instructions, ProgramLinkResolver links,
	unsigned checked) noexcept
{
	try {
		bool check_head = false;
		auto &stream = decoded_scratch();
		stream.clear();
		stream.reserve(maximum_instructions);
		auto &source = source_scratch();
		source.clear();
		auto current = normalize_instruction_pointer(mode, address);
		std::size_t bytes{};
		/* Only the instructions the block keeps are copied out, and only
		 * after they have decoded, so a failed decode's partial read and
		 * the bytes past the block's end never enter the source. */
		std::array<std::byte, maximum_instruction_bytes> consumed{};
		/* One window for the whole block: a run of instructions inside it is
		 * one fetch, and the bytes the decode uses are still the bytes the
		 * block is cached against. */
		FetchWindow window{};
		for (std::size_t index = 0; index < maximum_instructions; ++index) {
			const auto decoded = decode(memory, current, mode,
				consumed.data(), &window);
			if (decoded.status != DecodeStatus::decoded) {
				/* Preserve the side effects and retirement order of the valid
				 * prefix. Dispatch will decode the faulting instruction again
				 * after that prefix has executed.
				 */
				if (!stream.empty())
					break;
				if (decoded.status == DecodeStatus::fetch_fault)
					return {BuildStatus::fetch_fault, {},
						decoded.fault_address};
				return {BuildStatus::illegal_instruction, {}, current};
			}
			/* An instruction that could raise an exception the guest
			 * enabled ends the block before it, or is the whole block. */
			if (checked != 0 &&
				needs_check(decoded.instruction, checked)) {
				if (!stream.empty())
					break;
				check_head = true;
			}
			stream.push_back({current, decoded.instruction});
			source.insert(source.end(), consumed.data(),
				consumed.data() + decoded.instruction.length);
			current = advance_instruction_pointer(
				mode, current, decoded.instruction.length);
			bytes += decoded.instruction.length;
			if (check_head)
				break;
			if (decoded.instruction.terminal()) {
				/* A conditional branch's fall-through continues in this same
				 * block, so not taking the branch costs one dispatch instead
				 * of a link follow. Without a link resolver the branch has no
				 * taken record to enter and the block ends at it as before.
				 * The block still ends at an unconditional transfer or at the
				 * fetch window, so its size stays bounded. */
				if (decoded.instruction.operation !=
						Operation::jump_conditional ||
					links.resolve == nullptr)
					break;
			}
		}
		if (stream.empty())
			return {BuildStatus::illegal_instruction, {}, address};
		/* A gadget the bank has no form for ends the block before it, as a
		 * decode failure does: the prefix retires, and dispatch reports the
		 * refused instruction at its own address when it starts a block. */
		auto &words = native_scratch();
		if (check_head) {
			/* The instruction must have a form, as any other block's
			 * must, but the stream only leaves at it: a link that enters
			 * this block reaches the runtime, which runs it checked. */
			words.clear();
			if (append_native_stream(words, std::span<const Gadget>{stream},
				advance_instruction_pointer(mode,
					normalize_instruction_pointer(mode, address), bytes),
				mode, normalize_instruction_pointer(mode, address), {},
				StreamKind::block))
				return {BuildStatus::illegal_instruction, {}, address};
			words.clear();
			append_leave_stream(words, stream.front().address);
			return {BuildStatus::built,
				Program{mode, address,
					std::span<const Gadget>{stream},
					std::span<const std::byte>{source},
					std::span<const std::uintptr_t>{words}, true},
				0};
		}
		for (;;) {
			words.clear();
			const auto refused = append_native_stream(words,
				std::span<const Gadget>{stream},
				advance_instruction_pointer(mode,
					normalize_instruction_pointer(mode, address), bytes),
				mode, normalize_instruction_pointer(mode, address), links,
				StreamKind::block);
			if (!refused)
				break;
			if (*refused == 0)
				return {BuildStatus::illegal_instruction, {}, address};
			stream.resize(*refused);
			bytes = 0;
			for (const auto &gadget : stream)
				bytes += gadget.instruction.length;
			source.resize(bytes);
		}
		/* The saved bytes are the block: one instruction's encoding after
		 * another, ending where the block does. */
		return {BuildStatus::built,
			Program{mode, address,
				std::span<const Gadget>{stream},
				std::span<const std::byte>{source},
				std::span<const std::uintptr_t>{words}},
			0};
	} catch (...) {
		return {BuildStatus::allocation_failure, {}, 0};
	}
}

namespace {

[[nodiscard]] ExecuteResult run_raised(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, Executor &executor,
	std::span<const std::uintptr_t> native_stream,
	const NativeExecutionContext &context, std::size_t executed,
	std::size_t instruction_budget) noexcept;

/* Where a gadget that left with the stream pointer in the context's
 * exception stream was: the stream word just before it, in the stream this
 * run entered or in the cached block a link reached, which the context's
 * program begin names. */
struct StreamWord final {
	const Program *holder{};
	std::span<const std::uintptr_t> stream;
	StreamKind kind{};
	std::size_t word{};
};

[[nodiscard]] std::optional<StreamWord> locate_stream_word(
	const Program &program, Executor &executor,
	std::span<const std::uintptr_t> native_stream,
	const NativeExecutionContext &context) noexcept
{
	if (context.exception_stream == nullptr)
		return std::nullopt;
	const auto *word = context.exception_stream - 1;
	const Program *holder = &program;
	auto stream = native_stream;
	if (word < stream.data() || word >= stream.data() + stream.size()) {
		holder = executor.program_holding(context.program_begin,
			program.execution_mode(), word);
		if (holder == nullptr)
			return std::nullopt;
		stream = holder->native_stream();
	}
	/* Only a program's own stream is a block's; the one built from part of
	 * it for a budget-limited run is a fragment. */
	return StreamWord{holder, stream,
		stream.data() == holder->native_stream().data() ?
			StreamKind::block : StreamKind::fragment,
		static_cast<std::size_t>(word - stream.data())};
}

/* The instruction a baked PUSH or POP was running when it left, which has no
 * stream word to say. The block is decoded back out of its saved bytes, as a
 * replay does, but not kept: a stack fault is rare, and the decode costs what
 * a translation of the block does. */
struct StreamInstruction final {
	std::uint64_t address{};
	std::uint32_t length{};
};

[[nodiscard]] std::optional<StreamInstruction> recover_stream_instruction(
	const Program &program, Executor &executor,
	std::span<const std::uintptr_t> native_stream,
	const NativeExecutionContext &context) noexcept
{
	try {
		const auto located = locate_stream_word(
			program, executor, native_stream, context);
		if (!located)
			return std::nullopt;
		auto &gadgets = replay_scratch();
		if (!decode_block(*located->holder, gadgets))
			return std::nullopt;
		const auto index = instruction_at_word(gadgets, located->stream,
			located->word, located->holder->execution_mode(),
			located->holder->address(), located->kind);
		if (!index)
			return std::nullopt;
		return StreamInstruction{gadgets[*index].address,
			gadgets[*index].instruction.length};
	} catch (...) {
		return std::nullopt;
	}
}

/* Runs `native_stream`, the program's own stream or one built from its
 * instructions, for at most `limit` instructions of the caller's
 * `instruction_budget`. The host's floating-point exception flags are
 * cleared as the stream starts and gather the guest's sticky MXCSR flags,
 * which the run adds to MXCSR as it ends; with `WatchFloating` they are
 * reported, as MXCSR flags, in `floating_exceptions` instead, for the
 * runtime to correct.
 * A `Checked` run is one instruction the runtime checks itself, so the
 * gadgets check nothing (`checked_run_checks`); any other run has them check
 * what `gadget_checks` asks for. */
template<bool WatchFloating, bool Checked = false>
[[nodiscard, gnu::always_inline]] inline ExecuteResult run_stream(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, Executor &executor,
	std::span<const std::uintptr_t> native_stream, std::size_t limit,
	std::size_t instruction_budget,
	std::uint32_t *floating_exceptions = nullptr) noexcept
{
	NativeExecutionContext context{
		.memory = &memory,
		.tlb_entries = executor.tlb_entries(),
		.program_begin = program.address(),
		.program_end = program.address() + program.byte_count(),
		.instruction_limit = std::min<std::size_t>(limit, 4096),
		.target_links = executor.target_links(program.execution_mode()),
		.execution_context = executor.execution_context(),
		.sequence_context = &executor,
		.code_change_reader = [](void *opaque) noexcept {
			return static_cast<Executor *>(opaque)->current_code_changes();
		},
		.code_changes = executor.prepared_code_changes(),
		.code_publications = 0,
		.code_pages = executor.code_page_registry(),
		.stack_entry = executor.tlb_entries(),
	};
	/* RF is cleared as the next instruction starts, after the
	 * instruction-breakpoint check it exists to suppress. It reaches the
	 * engine only from IRET, which always leaves it, and in the flags the
	 * kernel hands over, so that instruction always starts here. A caller's
	 * exits before this run no instruction and keep it, as an interrupt
	 * taken before the instruction does. */
	cpu.rflags &= ~rflags_resume;
	cpu.exception_checks =
		Checked ? checked_run_checks(cpu) : gadget_checks(cpu);
	executor.enter_engine();
	clear_host_floating_exceptions();
	auto result = uml_x86_64_gadget_enter(
		native_stream.data(), &cpu, &context);
	if constexpr (WatchFloating)
		*floating_exceptions = host_floating_exceptions();
	else
		/* FPSR.IDC reports an input FPCR.FZ flushed, which the engine
		 * never sets; it is not x86's denormal operand. */
		cpu.mxcsr |= host_floating_exceptions() & ~mxcsr_denormal_operand;
	executor.leave_engine();
	/* A block that published a code write and then left the stream never
	 * reached the link-following test that settles its own publication, so
	 * the sequence it leaves behind reads as somebody else's change and cools
	 * every link in the address space. Settle it here on the same terms: the
	 * global sequence advanced by exactly this run's own publications, each of
	 * which already cooled the blocks it actually overlapped.
	 */
	if (context.code_publications != 0 &&
		context.code_publications <=
			std::numeric_limits<std::uint64_t>::max() - context.code_changes) {
		const auto settled = context.code_changes + context.code_publications;
		if (executor.current_code_changes() == settled)
			(void)executor.accept_code_changes(settled);
	}
	if (result.event == UML_X86_64_EVENT_EXCEPTION_CHECK) {
		if constexpr (Checked) {
			trace_fatal("checked-exception", program, cpu,
				instruction_budget, result.executed);
			return {::uml::cpu::EngineEvent::fatal, result.executed, {},
				false, false};
		} else {
			return run_raised(program, cpu, memory, executor,
				native_stream, context, result.executed,
				instruction_budget);
		}
	}
	if (result.event == UML_X86_64_EVENT_STREAM_FAULT ||
		result.event == UML_X86_64_EVENT_STREAM_RETIRED) {
		/* A baked PUSH or POP, which only a block's own stream holds and
		 * so never a checked run's, left without an address: it is a fault
		 * at the instruction, or it retired and the run goes on after it. */
		const bool retired = result.event == UML_X86_64_EVENT_STREAM_RETIRED;
		const auto where = Checked ? std::nullopt :
			recover_stream_instruction(program, executor, native_stream,
				context);
		if (!where) {
			trace_fatal("stream-recovery", program, cpu,
				instruction_budget, result.executed);
			return {::uml::cpu::EngineEvent::fatal, result.executed, {},
				false, false};
		}
		cpu.rip = retired ? advance_instruction_pointer(
			program.execution_mode(), where->address, where->length) :
			where->address;
		result.event = retired ? UML_X86_64_EVENT_CONTINUED :
			UML_X86_64_EVENT_MEMORY_FAULT;
	}
	if (result.event == UML_X86_64_EVENT_SEGMENT_FAULT) {
		ExecuteResult fault{::uml::cpu::EngineEvent::general_protection,
			result.executed, {}, false, false};
		fault.vector = context.segment_vector;
		fault.error = context.segment_error;
		return fault;
	}
	if (result.event == UML_X86_64_EVENT_SEGMENT_CHECK) {
		ExecuteResult check{::uml::cpu::EngineEvent::continued,
			result.executed, {}, false, false};
		check.segment_check = true;
		return check;
	}
	/* An instruction that loads the x87 control word or environment and
	 * unmasks an exception whose flag is set leaves the engine, and the
	 * exception is then pending: the status word shows it, and the next
	 * waiting instruction raises it. */
	if constexpr (!Checked)
		if (x87_pending(cpu) != 0)
			cpu.x87_status |= x87_status_pending;
	const bool repeat_yielded = result.event == UML_X86_64_EVENT_REPEAT_YIELD;
	const auto event = repeat_yielded ? ::uml::cpu::EngineEvent::continued :
		static_cast<::uml::cpu::EngineEvent>(result.event);
	if (event == ::uml::cpu::EngineEvent::continued &&
		context.indirect_miss != nullptr)
		executor.train_indirect(context.indirect_miss, cpu.rip, program.execution_mode());
	if (event == ::uml::cpu::EngineEvent::fatal)
		trace_fatal("native-entry", program, cpu, instruction_budget,
			result.executed);
	return {
		event,
		result.executed,
		context.fault,
		event == ::uml::cpu::EngineEvent::continued &&
			result.executed == instruction_budget,
		repeat_yielded,
	};
}

} // namespace

ExecuteResult execute(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt) noexcept

{
	try {
		Executor executor;
		executor.prepare(memory);
		return execute(program, cpu, memory, executor,
			instruction_budget, interrupt);
	} catch (...) {
		return {::uml::cpu::EngineEvent::fatal, 0, {}, false, false};
	}
}

ExecuteResult execute(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, Executor &executor,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt, bool single_step) noexcept
{
	if (program.native_stream().empty() ||
		cpu.execution_mode != program.execution_mode() ||
		cpu.rip != program.address()) {
		trace_fatal("precondition", program, cpu, instruction_budget);
		return {::uml::cpu::EngineEvent::fatal, 0, {}, false, false};
	}
	if (interrupt.pending())
		return {::uml::cpu::EngineEvent::interrupt, 0, {}, false, false};
	if (instruction_budget == 0)
		return {::uml::cpu::EngineEvent::continued, 0, {}, true, false};

	std::vector<std::uintptr_t> limited_stream;
	std::span<const std::uintptr_t> native_stream = program.native_stream();
	const bool limited = instruction_budget < program.instruction_count();
	if (limited || single_step) {
		try {
			const auto gadgets = program.replay_instructions();
			if (gadgets.size() != program.instruction_count()) {
				trace_fatal("limited-replay", program, cpu,
					instruction_budget);
				return {::uml::cpu::EngineEvent::fatal, 0, {},
					false, false};
			}
			if (single_step &&
				repeated_string(gadgets.front().instruction)) {
				limited_stream = build_repeat_step_stream(
					gadgets.front(), program.execution_mode(),
					program.address());
				native_stream = limited_stream;
			} else if (limited) {
				limited_stream = build_native_stream(
					gadgets.first(instruction_budget),
					gadgets[instruction_budget].address,
					program.execution_mode(), program.address());
				native_stream = limited_stream;
			}
		} catch (...) {
			trace_fatal("limited-stream", program, cpu,
				instruction_budget);
			return {::uml::cpu::EngineEvent::fatal, 0, {}, false, false};
		}
	}

	const auto result = run_stream<false>(program, cpu, memory, executor,
		native_stream, instruction_budget, instruction_budget);
	if (result.segment_check) [[unlikely]]
		return continue_segment_step(result, cpu, memory, executor,
			interrupt, 0, single_step);
	return result;
}

namespace {

/* The guest bytes a checked instruction may store before the exception the
 * runtime raises for it, which undoes the store: an x87 store, or
 * VCVTPS2PH to memory. */
struct SavedStore final {
	std::uint64_t address{};
	std::size_t bytes{};
	std::array<std::byte, 16> data{};
};

[[nodiscard]] SavedStore save_store(const CpuState &cpu,
	const Gadget &gadget, ::uml::memory::GuestAddressSpace &memory,
	unsigned checked)
{
	const auto &instruction = gadget.instruction;
	const Operand *operand = nullptr;
	std::size_t bytes{};
	if ((checked & check_x87) != 0 && x87_store_bytes(instruction) != 0) {
		operand = &instruction.source;
		bytes = x87_store_bytes(instruction);
	} else if ((checked & check_simd) != 0 && simd_floating(instruction) &&
		instruction.destination.kind == OperandKind::memory) {
		operand = &instruction.destination;
		bytes = static_cast<std::size_t>(instruction.destination.width);
	}
	SavedStore saved{};
	if (operand == nullptr || bytes > saved.data.size())
		return saved;
	saved.address = effective_address(cpu, gadget, *operand);
	if (memory.read(saved.address,
		std::span<std::byte>{saved.data}.first(bytes)))
		saved.bytes = bytes;
	return saved;
}

void restore_store(const SavedStore &saved,
	::uml::memory::GuestAddressSpace &memory)
{
	if (saved.bytes != 0)
		(void)memory.write(saved.address,
			std::span<const std::byte>{saved.data}.first(saved.bytes));
}

[[nodiscard]] ExecuteResult floating_point_trap(std::uint8_t vector,
	std::uint32_t error) noexcept
{
	ExecuteResult trap{::uml::cpu::EngineEvent::floating_point_trap, 0, {},
		false, false};
	trap.vector = vector;
	trap.error = error;
	return trap;
}

/* MXCSR's and the x87 status word's exception flags that x86 checks before
 * it computes a result: invalid operation, denormal operand and division by
 * zero. An unmasked one leaves the destination alone. */
constexpr std::uint32_t precomputation_exceptions = 0x07U;
constexpr std::uint32_t overflow_underflow = 0x18U;
constexpr std::uint8_t x87_error_vector = 16;
constexpr std::uint8_t simd_error_vector = 19;
/* The x87 status word's condition codes C0, C2 and C3, its C1, and its
 * stack fault flag. */
constexpr std::uint16_t x87_condition_codes = 0x4500U;
constexpr std::uint16_t x87_status_c1 = 0x0200U;
constexpr std::uint16_t x87_stack_fault = 0x0040U;
/* RFLAGS' ZF, PF and CF, which FCOMI sets, and OF, SF and AF, which it
 * clears. */
constexpr std::uint64_t x87_compare_rflags = 0x8d5U;

/* Runs instruction `index` of `program`, whose decoded instructions are
 * `gadgets`, alone and checked for the exceptions in `checked`: #MF for a
 * waiting x87 instruction with an unmasked exception pending, #AC for a
 * misaligned access, and after it #XM, or the pending #MF an x87
 * instruction leaves for the next one. The gadgets check nothing while it
 * runs. */
[[nodiscard]] ExecuteResult run_checked_instruction(const Program &program,
	std::span<const Gadget> gadgets, std::size_t index, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, Executor &executor,
	std::size_t instruction_budget, bool single_step, unsigned checked)
{
	const auto mode = program.execution_mode();
	const auto &first = gadgets[index];
	/* Where the one instruction's stream leaves when it falls off. */
	const auto final_rip = advance_instruction_pointer(mode,
		normalize_instruction_pointer(mode, first.address),
		first.instruction.length);
	const bool step_string = single_step &&
		repeated_string(first.instruction);

	const auto &instruction = first.instruction;
	/* The exits that follow raise their fault as the instruction starts,
	 * which is when RF clears. */
	cpu.rflags &= ~rflags_resume;
	if ((checked & check_x87) != 0 && waits_for_x87(instruction) &&
		x87_pending(cpu) != 0) {
		cpu.x87_status |= x87_status_pending;
		return floating_point_trap(x87_error_vector,
			cpu.x87_status | (std::uint32_t{cpu.x87_control} << 16U));
	}
	if ((checked & check_alignment) != 0 && misaligned_access(cpu, first))
		return {::uml::cpu::EngineEvent::memory_fault, 0,
			{0, ::uml::memory::MemoryAccess::read, true}, false, false};

	auto &stream = executor.checked_stream();
	if (step_string)
		stream = build_repeat_step_stream(first, mode, program.address());
	else
		build_native_stream_into(stream, gadgets.subspan(index, 1),
			final_rip, mode, program.address());
	const bool simd = (checked & check_simd) != 0 &&
		simd_floating(instruction);
	const bool x87 = (checked & check_x87) != 0 &&
		(x87_computes(instruction) || x87_loads_state(instruction));
	if (!simd && !x87)
		return run_stream<false, true>(program, cpu, memory, executor,
			stream, 1, instruction_budget);

	/* x86 raises its flags otherwise than the host does: the model computes
	 * them from the sources, which the instruction may overwrite. */
	const auto modelled = simd ? simd_fp_steps(cpu, first, memory) :
		std::optional<FpSteps>{};
	if (modelled && instruction.destination.kind != OperandKind::memory) {
		/* #XM is a fault: an unmasked exception leaves the instruction's
		 * destination and RIP as they were, with MXCSR's flags for it.
		 * Without a store to undo, the model decides it before the
		 * instruction runs, and a faulting one never does. */
		const auto outcome = x86_fp_settle(*modelled, cpu.mxcsr);
		if (outcome.fault) {
			cpu.mxcsr |= outcome.flags;
			return floating_point_trap(simd_error_vector, cpu.mxcsr);
		}
		/* The host's flags are not x86's. */
		std::uint32_t host{};
		const auto result = run_stream<true, true>(program, cpu, memory,
			executor, stream, 1, instruction_budget, &host);
		if (result.event == ::uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 1)
			cpu.mxcsr |= outcome.flags;
		return result;
	}
	const CpuState before = cpu;
	const auto saved = save_store(cpu, first, memory, checked);
	std::uint32_t raised{};
	const auto result = simd ?
		run_stream<true, true>(program, cpu, memory, executor, stream, 1,
			instruction_budget, &raised) :
		run_stream<false, true>(program, cpu, memory, executor, stream, 1,
			instruction_budget);
	if (result.event != ::uml::cpu::EngineEvent::continued ||
		result.retired_instructions != 1)
		return result;
	if (simd) {
		/* #XM is a fault: an unmasked exception leaves the instruction's
		 * destination and RIP as they were, with MXCSR's flags for it.
		 * One raised before the computation reports only those, as x86
		 * does. The host's flags stand for an instruction the model
		 * leaves to them. */
		FpSteps steps{};
		if (modelled) {
			steps = *modelled;
		} else {
			steps.before[0] = raised & precomputation_exceptions;
			steps.after[0] = raised & ~precomputation_exceptions;
		}
		const auto outcome = x86_fp_settle(steps, before.mxcsr);
		if (!outcome.fault) {
			cpu.mxcsr |= outcome.flags;
			return result;
		}
		cpu = before;
		restore_store(saved, memory);
		cpu.mxcsr |= outcome.flags;
		return floating_point_trap(simd_error_vector, cpu.mxcsr);
	}
	const auto pending = x87_pending(cpu);
	if (x87_loads_state(instruction)) {
		/* Unmasking a pending exception makes it the next waiting
		 * instruction's #MF, which the status word shows at once. */
		if (pending != 0)
			cpu.x87_status |= x87_status_pending;
		else
			cpu.x87_status &= static_cast<std::uint16_t>(
				~x87_status_pending);
		return result;
	}
	if (pending == 0)
		return result;
	/* x87 reports its exception at the next waiting instruction. One
	 * checked before the computation -- invalid operation, denormal
	 * operand, division by zero -- leaves the registers, the stack and
	 * memory alone and raises none of a result's exceptions, except that
	 * FLD of a single or double completes beside a denormal operand and a
	 * comparison still sets its condition codes or flags. An overflow or
	 * underflow of a store stores nothing and raises no precision. The
	 * rest leave their result. */
	const bool precomputation = (pending & precomputation_exceptions) != 0;
	const bool store_overflow = !precomputation &&
		x87_store_bytes(instruction) != 0 &&
		(pending & overflow_underflow) != 0;
	if ((precomputation && !(x87_converts_load(instruction) &&
			(pending & ~0x02U) == 0)) || store_overflow) {
		const auto rip = cpu.rip;
		const auto status = cpu.x87_status;
		const auto rflags = cpu.rflags;
		const auto compare = x87_compare(instruction);
		cpu = before;
		cpu.rip = rip;
		restore_store(saved, memory);
		auto kept = static_cast<std::uint16_t>(before.x87_status &
			(x87_condition_codes | x87_exception_flags));
		if (store_overflow) {
			kept |= static_cast<std::uint16_t>(status & overflow_underflow);
		} else {
			kept |= static_cast<std::uint16_t>(status &
				(precomputation_exceptions | x87_stack_fault));
			/* A stack fault's C1 tells overflow from underflow. */
			if ((pending & 0x01U) != 0 && (status & x87_stack_fault) != 0)
				kept |= static_cast<std::uint16_t>(status & x87_status_c1);
			if (compare == X87Compare::status)
				kept = static_cast<std::uint16_t>(
					(kept & ~x87_condition_codes) |
					(status & x87_condition_codes));
			else if (compare == X87Compare::flags)
				cpu.rflags = (cpu.rflags & ~x87_compare_rflags) |
					(rflags & x87_compare_rflags);
		}
		cpu.x87_status = kept;
	}
	cpu.x87_status |= x87_status_pending;
	return result;
}

/* A gadget left before it committed an instruction that raised an
 * exception the guest unmasked. The stream word just before the context's
 * exception stream is one of that instruction's: in the stream this run
 * entered, or in a cached block a link reached, which is the block the
 * context names. The instruction runs again, checked, and the run reports
 * both parts. */
ExecuteResult run_raised(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, Executor &executor,
	std::span<const std::uintptr_t> native_stream,
	const NativeExecutionContext &context, std::size_t executed,
	std::size_t instruction_budget) noexcept
{
	cpu.exception_raised = 0;
	try {
		const auto located = locate_stream_word(
			program, executor, native_stream, context);
		if (!located) {
			trace_fatal("raised-block", program, cpu,
				instruction_budget, executed);
			return {::uml::cpu::EngineEvent::fatal, executed, {},
				false, false};
		}
		const auto *holder = located->holder;
		const auto gadgets = holder->replay_instructions();
		const auto index = gadgets.size() == holder->instruction_count() ?
			instruction_at_word(gadgets, located->stream, located->word,
				holder->execution_mode(), holder->address(),
				located->kind) :
			std::nullopt;
		if (!index || executed >= instruction_budget) {
			trace_fatal("raised-instruction", *holder, cpu,
				instruction_budget, executed);
			return {::uml::cpu::EngineEvent::fatal, executed, {},
				false, false};
		}
		cpu.rip = gadgets[*index].address;
		auto result = run_checked_instruction(*holder, gadgets, *index,
			cpu, memory, executor, instruction_budget - executed, false,
			enabled_exceptions(cpu));
		result.retired_instructions += executed;
		result.instruction_budget_exhausted =
			result.event == ::uml::cpu::EngineEvent::continued &&
			result.retired_instructions == instruction_budget;
		return result;
	} catch (...) {
		trace_fatal("raised-stream", program, cpu, instruction_budget,
			executed);
		return {::uml::cpu::EngineEvent::fatal, executed, {}, false, false};
	}
}

} // namespace

ExecuteResult segment_step(CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, Executor &executor,
	const ::uml::cpu::InterruptToken &interrupt, unsigned checked,
	std::uint64_t string_elements) noexcept
{
	/* An instruction the bypass let through never asks again. */
	if (cpu.segment_bypass != 0)
		return {::uml::cpu::EngineEvent::fatal, 0, {}, false, false};
	auto step = check_segment_step(cpu, memory, executor.execution_context(),
		string_elements);
	if (step.fetch_fault)
		return {::uml::cpu::EngineEvent::memory_fault, 0, step.fault,
			false, false};
	if (step.illegal)
		return {::uml::cpu::EngineEvent::illegal_instruction, 0, {}, false,
			false};
	switch (step.result.outcome) {
	case SegmentOutcome::retired:
	case SegmentOutcome::retired_changed:
		return {step.result.trap_armed ? ::uml::cpu::EngineEvent::trap_flag :
			::uml::cpu::EngineEvent::continued, 1, {}, false, false};
	case SegmentOutcome::yielded:
		return {::uml::cpu::EngineEvent::continued, 0, {}, false, true};
	case SegmentOutcome::memory_fault:
		return {::uml::cpu::EngineEvent::memory_fault, 0, step.fault, false,
			false};
	case SegmentOutcome::fault: {
		ExecuteResult fault{::uml::cpu::EngineEvent::general_protection, 0,
			{}, false, false};
		fault.vector = step.result.fault.vector;
		fault.error = step.result.fault.error;
		return fault;
	}
	case SegmentOutcome::run:
		break;
	}
	const Gadget gadget{cpu.rip, step.instruction};
	const SegmentBypass bypass{cpu};
	return execute_step(gadget, cpu, memory, executor, interrupt, checked);
}

ExecuteResult continue_segment_step(const ExecuteResult &prior,
	CpuState &cpu, ::uml::memory::GuestAddressSpace &memory,
	Executor &executor, const ::uml::cpu::InterruptToken &interrupt,
	unsigned checked, bool single) noexcept
{
	auto stepped = segment_step(cpu, memory, executor, interrupt, checked,
		single ? 1U : string_repeat_budget);
	stepped.retired_instructions += prior.retired_instructions;
	return stepped;
}

ExecuteResult execute_step(const Gadget &gadget, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, Executor &executor,
	const ::uml::cpu::InterruptToken &interrupt, unsigned checked) noexcept
{
	if (interrupt.pending())
		return {::uml::cpu::EngineEvent::interrupt, 0, {}, false, false};
	try {
		const auto mode = cpu.execution_mode;
		const auto length = gadget.instruction.length;
		std::array<std::byte, maximum_instruction_bytes> source{};
		if (!memory.fetch(gadget.address, std::span{source}.first(length)))
			return {::uml::cpu::EngineEvent::fatal, 0, {}, false, false};
		const auto stream = build_native_stream({&gadget, 1},
			advance_instruction_pointer(mode, gadget.address, length), mode,
			normalize_instruction_pointer(mode, gadget.address));
		const Program program{mode, gadget.address, {&gadget, 1},
			std::span<const std::byte>{source}.first(length), stream};
		if (checked != 0)
			return run_checked_instruction(program, {&gadget, 1}, 0, cpu,
				memory, executor, 1, false,
				checked | enabled_exceptions(cpu));
		return run_stream<false>(program, cpu, memory, executor,
			program.native_stream(), 1, 1);
	} catch (...) {
		return {::uml::cpu::EngineEvent::fatal, 0, {}, false, false};
	}
}

ExecuteResult execute_checked(const Program &program, CpuState &cpu,
	::uml::memory::GuestAddressSpace &memory, Executor &executor,
	std::size_t instruction_budget,
	const ::uml::cpu::InterruptToken &interrupt, bool single_step,
	unsigned checked) noexcept
{
	if (program.native_stream().empty() ||
		cpu.execution_mode != program.execution_mode() ||
		cpu.rip != program.address()) {
		trace_fatal("checked-precondition", program, cpu,
			instruction_budget);
		return {::uml::cpu::EngineEvent::fatal, 0, {}, false, false};
	}
	if (interrupt.pending())
		return {::uml::cpu::EngineEvent::interrupt, 0, {}, false, false};
	if (instruction_budget == 0)
		return {::uml::cpu::EngineEvent::continued, 0, {}, true, false};
	/* The build left every instruction that needs a check out of this
	 * block, and its links reach only blocks built the same way. */
	if (!program.check_head())
		return execute(program, cpu, memory, executor, instruction_budget,
			interrupt, single_step);
	try {
		const auto gadgets = program.replay_instructions();
		if (gadgets.size() != program.instruction_count()) {
			trace_fatal("checked-replay", program, cpu,
				instruction_budget);
			return {::uml::cpu::EngineEvent::fatal, 0, {},
				false, false};
		}
		/* The block was split for `checked`; the instruction is checked
		 * for every exception the guest enables. */
		const auto all = checked | enabled_exceptions(cpu);
		const auto result = run_checked_instruction(program, gadgets, 0,
			cpu, memory, executor, instruction_budget, single_step, all);
		if (result.segment_check) [[unlikely]]
			return continue_segment_step(result, cpu, memory, executor,
				interrupt, all, single_step);
		return result;
	} catch (...) {
		trace_fatal("checked-stream", program, cpu, instruction_budget);
		return {::uml::cpu::EngineEvent::fatal, 0, {}, false, false};
	}
}

} // namespace uml::cpu::x86_64::gadgets
