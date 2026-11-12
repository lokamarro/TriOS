/* SPDX-License-Identifier: GPL-2.0-or-later */
/* One source, three runners: the gadget engine, the JIT (UML_FEX_ASM_JIT) and
 * the tiered handover between them (UML_FEX_ASM_SWITCH, a JIT runner that
 * also links the gadgets). */
#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/segments.hpp"
#if UML_FEX_ASM_SWITCH && !UML_FEX_ASM_JIT
#error "UML_FEX_ASM_SWITCH extends the JIT runner"
#endif
#if !UML_FEX_ASM_JIT || UML_FEX_ASM_SWITCH
#define UML_FEX_ASM_GADGETS 1
#else
#define UML_FEX_ASM_GADGETS 0
#endif
#if UML_FEX_ASM_JIT
#include "cpu/x86_64/jit/fex_engine.hpp"
#include "fex_emitted_registers.hpp"
#include "platform/apple/host_features.hpp"
#endif
#if UML_FEX_ASM_GADGETS
#include "cpu/x86_64/gadgets/program.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#endif
#include "memory/guest_address_space.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t page_size = 4096;
constexpr std::uint64_t code_address = 0x1'0000;
constexpr std::uint64_t scratch_address = 0xe000'0000;
constexpr std::uint64_t sib_address = 0xe800'0000;
constexpr std::size_t instruction_limit = 1'000'000;
#if UML_FEX_ASM_GADGETS
/* The address-space generation cached runs share their code-page registry
 * under; each case's executor releases it before the next case starts. */
constexpr std::uint64_t cached_generation = 1;
#endif

struct Region final {
	std::uint64_t begin{};
	std::uint64_t end{};
};

class SparseMemory final : public uml::memory::GuestAddressSpace {
public:
	[[nodiscard]] bool map(std::uint64_t address, std::uint64_t size)
	{
		if (size == 0 || address >
			std::numeric_limits<std::uint64_t>::max() - size)
			return false;
		regions_.push_back({address, address + size});
		std::sort(regions_.begin(), regions_.end(),
			[](const Region &left, const Region &right) {
				return left.begin < right.begin;
			});
		std::vector<Region> merged;
		for (const auto &region : regions_) {
			if (merged.empty() || region.begin > merged.back().end)
				merged.push_back(region);
			else
				merged.back().end = std::max(merged.back().end, region.end);
		}
		regions_ = std::move(merged);
		return true;
	}

	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		return transfer_read(address, destination,
			uml::memory::MemoryAccess::read, true);
	}

	[[nodiscard]] bool fetch(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		return transfer_read(address, destination,
			uml::memory::MemoryAccess::execute, true);
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> source) override
	{
		if (!mapped(address, source.size())) {
			set_fault(address, uml::memory::MemoryAccess::write);
			return false;
		}
		std::size_t offset{};
		while (offset < source.size()) {
			const auto current = address + offset;
			const auto page_number = current / page_size;
			const auto page_offset = static_cast<std::size_t>(
				current % page_size);
			const auto count = std::min(source.size() - offset,
				static_cast<std::size_t>(page_size) - page_offset);
			auto [iterator, inserted] = pages_.try_emplace(page_number);
			if (inserted)
				iterator->second.fill(std::byte{});
			std::memcpy(iterator->second.data() + page_offset,
				source.data() + offset, count);
			offset += count;
		}
		return true;
	}

	[[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
		std::size_t size, uml::memory::MemoryAccess access) override
	{
		if (size == 0 || size > page_size - (address % page_size) ||
			!mapped(address, size)) {
			set_fault(address, access);
			return nullptr;
		}
		const auto page_number = address / page_size;
		auto [iterator, inserted] = pages_.try_emplace(page_number);
		if (inserted)
			iterator->second.fill(std::byte{});
		return iterator->second.data() +
			static_cast<std::size_t>(address % page_size);
	}

	/* Repository cases run on the product's code-page model: every mapped
	 * page may hold code, as under READ_IMPLIES_EXEC, and only a code
	 * publication moves a page's stamp and the address space's code-change
	 * sequence, so a cached translation survives exactly the stores nobody
	 * published. */
	void model_code_pages(std::uint64_t *code_changes) noexcept
	{
		code_changes_ = code_changes;
	}

	[[nodiscard]] bool is_executable(std::uint64_t address) noexcept override
	{
		return code_changes_ != nullptr && mapped(address, 1);
	}

	[[nodiscard]] bool code_page_stamp(std::uint64_t address, void *,
		uml::memory::CodePageStamp &stamp) noexcept override
	{
		if (code_changes_ == nullptr || !mapped(address, 1))
			return false;
		const auto page_number = address / page_size;
		const auto version = code_versions_.find(page_number);
		stamp = {page_number + 1U,
			version == code_versions_.end() ? 0 : version->second};
		return true;
	}

	void invalidate_code(std::uint64_t address) override
	{
		if (code_changes_ == nullptr)
			return;
		++code_versions_[address / page_size];
		++*code_changes_;
	}

	[[nodiscard]] uml::memory::MemoryFault last_fault() const noexcept override
	{
		return fault_;
	}

	[[nodiscard]] bool has_fault() const noexcept override { return faulted_; }

	[[nodiscard]] std::vector<std::byte> snapshot(
		std::uint64_t address, std::size_t size) const
	{
		std::vector<std::byte> bytes;
		bytes.reserve(size);
		for (std::size_t offset = 0; offset < size; ++offset) {
			if (!mapped(address + offset, 1))
				break;
			const auto current = address + offset;
			const auto iterator = pages_.find(current / page_size);
			bytes.push_back(iterator == pages_.end() ? std::byte{} :
				iterator->second[static_cast<std::size_t>(current % page_size)]);
		}
		return bytes;
	}

private:
	struct alignas(page_size) Page final {
		[[nodiscard]] std::byte *data() noexcept { return bytes.data(); }
		[[nodiscard]] const std::byte *data() const noexcept
		{
			return bytes.data();
		}
		[[nodiscard]] std::byte &operator[](std::size_t index) noexcept
		{
			return bytes[index];
		}
		[[nodiscard]] const std::byte &operator[](
			std::size_t index) const noexcept
		{
			return bytes[index];
		}
		void fill(std::byte value) noexcept { bytes.fill(value); }

		std::array<std::byte, page_size> bytes{};
	};

	[[nodiscard]] bool mapped(std::uint64_t address,
		std::size_t size) const noexcept
	{
		if (size == 0)
			return true;
		if (address > std::numeric_limits<std::uint64_t>::max() - size)
			return false;
		const auto end = address + size;
		return std::any_of(regions_.begin(), regions_.end(),
			[address, end](const Region &region) {
				return address >= region.begin && end <= region.end;
			});
	}

	[[nodiscard]] bool transfer_read(std::uint64_t address,
		std::span<std::byte> destination, uml::memory::MemoryAccess access,
		bool record_fault)
	{
		if (!mapped(address, destination.size())) {
			if (record_fault)
				set_fault(address, access);
			return false;
		}
		std::size_t offset{};
		while (offset < destination.size()) {
			const auto current = address + offset;
			const auto page_number = current / page_size;
			const auto page_offset = static_cast<std::size_t>(
				current % page_size);
			const auto count = std::min(destination.size() - offset,
				static_cast<std::size_t>(page_size) - page_offset);
			const auto iterator = pages_.find(page_number);
			if (iterator == pages_.end())
				std::fill_n(destination.data() + offset, count, std::byte{});
			else
				std::memcpy(destination.data() + offset,
					iterator->second.data() + page_offset, count);
			offset += count;
		}
		return true;
	}

	void set_fault(std::uint64_t address,
		uml::memory::MemoryAccess access) noexcept
	{
		fault_ = {address, access};
		faulted_ = true;
	}

	std::vector<Region> regions_;
	std::map<std::uint64_t, Page> pages_;
	std::map<std::uint64_t, std::uint64_t> code_versions_;
	std::uint64_t *code_changes_{};
	uml::memory::MemoryFault fault_{};
	bool faulted_{};
};

/* How a case reaches its blocks. Upstream FEX programs build each block
 * afresh; repository cases dispatch through the executor's program cache on
 * the product's code-page model, so they can also hold the engine to its
 * code-publication contract. */
enum class Dispatch {
	fresh,
	cached,
};

struct CodeSequences final {
	std::uint64_t mapping_changes{1};
	std::uint64_t code_changes{1};
};

#if UML_FEX_ASM_GADGETS
[[nodiscard]] uml::cpu::x86_64::gadgets::ProgramCacheSequences
read_code_sequences(void *opaque) noexcept
{
	const auto &sequences = *static_cast<const CodeSequences *>(opaque);
	return {
		.mapping_changes = sequences.mapping_changes,
		.code_changes = sequences.code_changes,
		.interrupted = false,
	};
}
#endif

struct RegisterExpectation final {
	std::uint64_t key{};
	std::vector<std::uint64_t> values;
};

struct MemoryData final {
	std::uint64_t address{};
	std::vector<std::byte> bytes;
};

struct Configuration final {
	std::vector<Region> memory_regions;
	std::vector<RegisterExpectation> registers;
	std::vector<MemoryData> memory_data;
	uml::cpu::x86_64::ExecutionMode mode{
		uml::cpu::x86_64::ExecutionMode::long64};
};

[[nodiscard]] bool range_valid(std::span<const std::byte> bytes,
	std::size_t offset, std::size_t size) noexcept
{
	return offset <= bytes.size() && size <= bytes.size() - offset;
}

/* Reads a little-endian `Value` at `offset`, or fails when it would extend
 * past the end of `bytes`. */
template<std::unsigned_integral Value>
[[nodiscard]] bool read_le(std::span<const std::byte> bytes,
	std::size_t offset, Value &value) noexcept
{
	if (!range_valid(bytes, offset, sizeof(value)))
		return false;
	value = 0;
	for (std::size_t index = 0; index < sizeof(value); ++index)
		value |= static_cast<Value>(static_cast<Value>(
			std::to_integer<std::uint8_t>(bytes[offset + index]))
			<< (index * 8U));
	return true;
}

[[nodiscard]] bool parse_configuration(std::span<const std::byte> bytes,
	Configuration &configuration, std::string &error)
{
	constexpr std::size_t header_size = 76;
	if (bytes.size() < header_size) {
		error = "configuration header is truncated";
		return false;
	}
	std::uint32_t mode{};
	std::uint32_t region_offset{};
	std::uint32_t region_count{};
	std::uint32_t register_offset{};
	std::uint32_t register_count{};
	std::uint32_t memory_offset{};
	std::uint32_t memory_count{};
	std::uint32_t environment_offset{};
	std::uint32_t environment_count{};
	if (!read_le(bytes, 36, mode) || !read_le(bytes, 44, region_offset) ||
		!read_le(bytes, 48, region_count) ||
		!read_le(bytes, 52, register_offset) ||
		!read_le(bytes, 56, register_count) ||
		!read_le(bytes, 60, memory_offset) ||
		!read_le(bytes, 64, memory_count) ||
		!read_le(bytes, 68, environment_offset) ||
		!read_le(bytes, 72, environment_count)) {
		error = "configuration header fields are truncated";
		return false;
	}
	if (mode > 1) {
		error = "configuration has an unsupported FEX execution mode";
		return false;
	}
	configuration.mode = mode == 0 ?
		uml::cpu::x86_64::ExecutionMode::compat32 :
		uml::cpu::x86_64::ExecutionMode::long64;

	auto cursor = static_cast<std::size_t>(region_offset);
	for (std::uint32_t index = 0; index < region_count; ++index) {
		Region region{};
		std::uint64_t size{};
		if (!read_le(bytes, cursor, region.begin) ||
			!read_le(bytes, cursor + 8, size) || size == 0 ||
			region.begin > std::numeric_limits<std::uint64_t>::max() - size) {
			error = "invalid memory-region record";
			return false;
		}
		region.end = region.begin + size;
		configuration.memory_regions.push_back(region);
		cursor += 16;
	}

	cursor = static_cast<std::size_t>(register_offset);
	for (std::uint32_t index = 0; index < register_count; ++index) {
		std::uint32_t value_count{};
		RegisterExpectation expectation{};
		if (!read_le(bytes, cursor, value_count) ||
			!read_le(bytes, cursor + 4, expectation.key) ||
			expectation.key == 0 ||
			(expectation.key & (expectation.key - 1U)) != 0 ||
			value_count == 0 || value_count > 4) {
			error = "invalid register-data record";
			return false;
		}
		cursor += 12;
		for (std::uint32_t value_index = 0;
			value_index < value_count; ++value_index) {
			std::uint64_t value{};
			if (!read_le(bytes, cursor, value)) {
				error = "truncated register-data value";
				return false;
			}
			expectation.values.push_back(value);
			cursor += 8;
		}
		configuration.registers.push_back(std::move(expectation));
	}

	cursor = static_cast<std::size_t>(memory_offset);
	for (std::uint32_t index = 0; index < memory_count; ++index) {
		MemoryData data{};
		std::uint32_t size{};
		if (!read_le(bytes, cursor, data.address) ||
			!read_le(bytes, cursor + 8, size) ||
			!range_valid(bytes, cursor + 12, size)) {
			error = "invalid memory-data record";
			return false;
		}
		data.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor + 12),
			bytes.begin() + static_cast<std::ptrdiff_t>(cursor + 12 + size));
		configuration.memory_data.push_back(std::move(data));
		cursor += 12 + size;
	}

	cursor = static_cast<std::size_t>(environment_offset);
	for (std::uint32_t index = 0; index < environment_count; ++index) {
		if (cursor >= bytes.size()) {
			error = "environment data is truncated";
			return false;
		}
		const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(cursor);
		const auto key_end = std::find(begin, bytes.end(), std::byte{});
		if (key_end == bytes.end()) {
			error = "truncated environment key";
			return false;
		}
		const auto value_begin = key_end + 1;
		const auto value_end = std::find(value_begin, bytes.end(), std::byte{});
		if (value_end == bytes.end()) {
			error = "truncated environment value";
			return false;
		}
		const std::string key(reinterpret_cast<const char *>(
			bytes.data() + cursor),
			static_cast<std::size_t>(key_end - begin));
		const std::string value(reinterpret_cast<const char *>(&*value_begin),
			static_cast<std::size_t>(value_end - value_begin));
		/* FEX's reduced-precision x87 mode computes in doubles; the
		 * engine has only full 80-bit x87, so a case asking for it
		 * could not mean what it says. */
		if (key == "FEX_X87REDUCEDPRECISION" && value != "0") {
			error = "FEX_X87REDUCEDPRECISION selects a reduced-precision "
				"x87 mode the engine does not implement";
			return false;
		}
		cursor = static_cast<std::size_t>(value_end - bytes.begin()) + 1U;
	}
	return true;
}

[[nodiscard]] bool load_file(const char *path, std::vector<std::byte> &bytes,
	std::string &error)
{
	std::ifstream stream{path, std::ios::binary | std::ios::ate};
	if (!stream) {
		error = "cannot open input file";
		return false;
	}
	const auto end = stream.tellg();
	if (end < 0) {
		error = "cannot determine input size";
		return false;
	}
	const auto size = static_cast<std::size_t>(end);
	bytes.resize(size);
	stream.seekg(0);
	if (size != 0 && !stream.read(reinterpret_cast<char *>(bytes.data()),
		static_cast<std::streamsize>(size))) {
		error = "cannot read input file";
		return false;
	}
	return true;
}

[[nodiscard]] std::string hex_bytes(std::span<const std::byte> bytes)
{
	std::ostringstream output;
	output << std::hex << std::setfill('0');
	for (const auto byte : bytes)
		output << std::setw(2)
			<< static_cast<unsigned>(std::to_integer<std::uint8_t>(byte));
	return output.str();
}

/* The known failure class of the record running now, when an exclusion
 * list names it. */
std::string_view excluded_class;

#if UML_FEX_ASM_SWITCH
/* --switch-sweep runs every case again at each switch point (see
 * GadgetTier). */
bool switch_sweep = false;
/* Instructions the gadgets retire before the JIT takes over; zero is the
 * JIT alone. */
std::size_t switch_after = 0;
/* Whether the case reached the JIT after the gadgets: false ends the sweep,
 * since a larger switch point cannot change the run. */
bool switch_taken = false;
#endif

/* The last result a case reported, which --retirement checks instead of
 * printing. */
struct ReportedResult final {
	std::string status;
	std::size_t retired{};
	std::uint64_t address{};
	/* RAX where a fault or other stop left it. */
	std::uint64_t rax{};
};
ReportedResult reported;
bool capture_results = false;

void print_result(std::string_view status, std::size_t retired,
	std::uint64_t address, std::span<const std::byte> opcode,
	std::string_view detail, std::string_view test = {})
{
	reported = {std::string{status}, retired, address};
	if (capture_results)
		return;
	std::cout << "status=" << status << '\t';
	if (!test.empty())
		std::cout << "test=" << test << '\t';
	if (!excluded_class.empty())
		std::cout << "class=" << excluded_class << '\t';
#if UML_FEX_ASM_SWITCH
	if (switch_after != 0)
		std::cout << "switch=" << switch_after << '\t';
#endif
	std::cout
		<< "retired=" << retired << '\t'
		<< "address=0x" << std::hex << address << std::dec << '\t'
		<< "opcode=" << hex_bytes(opcode) << '\t'
		<< "detail=" << detail << '\n';
}

/* FEX expectation keys, K-trace register masks and register_name() all index
 * the general registers in this order, and the K-trace and compact RFLAGS
 * encodings index the flags in this one. */
constexpr std::array<uml::cpu::x86_64::Register, 16> pack_registers{
	uml::cpu::x86_64::Register::rax, uml::cpu::x86_64::Register::rbx,
	uml::cpu::x86_64::Register::rcx, uml::cpu::x86_64::Register::rdx,
	uml::cpu::x86_64::Register::rsi, uml::cpu::x86_64::Register::rdi,
	uml::cpu::x86_64::Register::rbp, uml::cpu::x86_64::Register::rsp,
	uml::cpu::x86_64::Register::r8, uml::cpu::x86_64::Register::r9,
	uml::cpu::x86_64::Register::r10, uml::cpu::x86_64::Register::r11,
	uml::cpu::x86_64::Register::r12, uml::cpu::x86_64::Register::r13,
	uml::cpu::x86_64::Register::r14, uml::cpu::x86_64::Register::r15};
constexpr std::array<std::uint64_t, 7> pack_flags{
	uml::cpu::x86_64::rflags_carry, uml::cpu::x86_64::rflags_parity,
	uml::cpu::x86_64::rflags_auxiliary, uml::cpu::x86_64::rflags_zero,
	uml::cpu::x86_64::rflags_sign, uml::cpu::x86_64::rflags_direction,
	uml::cpu::x86_64::rflags_overflow};

[[nodiscard]] const char *register_name(unsigned index) noexcept
{
	constexpr std::array<const char *, 16> names{
		"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP",
		"R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};
	return index < names.size() ? names[index] : "unknown";
}

/* FEX builds every 32-bit test to start with "mov eax, 0x17; mov ds, ax;
 * mov es, ax" (unittests/32Bit_ASM/CMakeLists.txt): selector 0x17 names
 * LDT entry 2, which the tests take to be flat data, as FEX checks no
 * selector. An i386 task gets that LDT, with the entry modify_ldt() writes
 * for a flat 32-bit data segment. */
constexpr std::size_t fex_test_data_entry = 2;
constexpr std::uint64_t flat_32bit_data = 0x00cf'f300'0000'ffffULL;
const std::array<std::uint64_t, 8192> fex_test_ldt = [] {
	std::array<std::uint64_t, 8192> table{};
	table[fex_test_data_entry] = flat_32bit_data;
	return table;
}();

/* The segments Linux gives a task at execve: the flat 64-bit or 32-bit
 * user code segment and the flat data segment as SS, and as DS and ES for
 * an i386 task; the execution mode follows CS. */
void linux_segments(uml::cpu::x86_64::CpuState &cpu,
	uml::cpu::x86_64::ExecutionMode mode)
{
	const bool compat = mode == uml::cpu::x86_64::ExecutionMode::compat32;
	cpu.segment_selectors = {
		static_cast<std::uint16_t>(compat ? 0x2b : 0),
		static_cast<std::uint16_t>(compat ? 0x23 : 0x33), 0x2b,
		static_cast<std::uint16_t>(compat ? 0x2b : 0), 0, 0};
	cpu.ldt = compat ? fex_test_ldt.data() : nullptr;
	(void)uml::cpu::x86_64::enter_segments(cpu);
}

[[nodiscard]] bool compare_registers(const Configuration &configuration,
	const uml::cpu::x86_64::CpuState &cpu, std::string &status,
	std::string &detail)
{
	for (const auto &expectation : configuration.registers) {
		const auto index = std::countr_zero(expectation.key);
		if (index == 0) {
			if (expectation.values.size() != 1 ||
				cpu.rip != expectation.values.front()) {
				status = "result_mismatch";
				detail = "RIP";
				return false;
			}
		} else if (index >= 1 && index <= 16) {
			const auto register_index = index - 1;
			if (expectation.values.size() != 1 ||
				cpu.gpr(pack_registers[register_index]) !=
					expectation.values.front()) {
				status = "result_mismatch";
				std::ostringstream mismatch;
				mismatch << register_name(register_index) <<
					" expected=0x" << std::hex <<
					(expectation.values.empty() ? 0 :
					 expectation.values.front()) << " actual=0x" <<
					cpu.gpr(pack_registers[register_index]);
				detail = mismatch.str();
				return false;
			}
		} else if (index >= 17 && index <= 32) {
			const auto vector_index = index - 17;
			for (std::size_t lane = 0;
				lane < expectation.values.size(); ++lane) {
				std::uint64_t actual{};
				std::memcpy(&actual,
					cpu.vectors[vector_index].data() + lane * sizeof(actual),
					sizeof(actual));
				if (actual != expectation.values[lane]) {
					status = "result_mismatch";
					std::ostringstream mismatch;
					mismatch << "XMM" << vector_index << '[' << lane <<
						"] expected=0x" << std::hex <<
						expectation.values[lane] << " actual=0x" << actual;
					detail = mismatch.str();
					return false;
				}
			}
		} else if (index == 33 || index == 34) {
			const auto actual = index == 33 ? cpu.gs_base : cpu.fs_base;
			if (expectation.values.size() != 1 ||
				actual != expectation.values.front()) {
				status = "result_mismatch";
				detail = index == 33 ? "GS" : "FS";
				return false;
			}
		} else if (index >= 35 && index <= 42) {
			const auto mmx_index = index - 35;
			bool matched = false;
			if (expectation.values.size() == 1) {
				matched = cpu.mmx[mmx_index] == expectation.values.front();
			} else if (expectation.values.size() == 2) {
				matched = cpu.x87[mmx_index].significand ==
					expectation.values[0] &&
					cpu.x87[mmx_index].sign_exponent ==
					static_cast<std::uint16_t>(expectation.values[1]);
			}
			if (!matched) {
				status = "result_mismatch";
				detail = "MM" + std::to_string(mmx_index);
				return false;
			}
		} else {
			status = "malformed_input";
			detail = "unknown register key";
			return false;
		}
	}
	status = "passed";
	detail = "reached HLT with expected state";
	return true;
}

/* HLT, the end marker, at RIP ends a case: its registers are compared with
 * the expected ones and the result reported. Returns the exit code, or
 * nothing when the instruction at RIP, whose bytes are `opcode`, is not the
 * marker. */
[[nodiscard]] std::optional<int> finish_at_marker(
	const Configuration &configuration,
	const uml::cpu::x86_64::CpuState &cpu, std::span<const std::byte> opcode,
	std::size_t retired, bool quiet_success, std::string_view test)
{
	if (opcode.empty() || opcode.front() != std::byte{0xf4})
		return std::nullopt;
	std::string status;
	std::string detail;
	const auto passed = compare_registers(configuration, cpu, status, detail);
	if (!passed || !quiet_success)
		print_result(status, retired, cpu.rip, opcode, detail, test);
	return passed ? 0 : 1;
}

/* Answers the system calls a few tests make for their own sake, which keeps
 * Linux ABI emulation outside the CPU engines; returns whether `cpu`, stopped
 * at a system call, was one of them.
 *
 * One upstream decoder test uses Linux time(2) only to prove that SYSCALL
 * reached a live boundary, and a repository case to read the flags SYSCALL
 * saves in R11. FEX's compatibility-ABI regression invokes 32-bit
 * clock_gettime(2) through INT 80h and observes only the successful return.
 * Its inline-syscall decoder regression issues getrandom(2) with a
 * zero-length buffer and needs only the architecturally successful boundary.
 */
[[nodiscard]] bool serve_test_system_call(
	uml::cpu::x86_64::CpuState &cpu) noexcept
{
	using uml::cpu::x86_64::Register;
	const auto low = [&](Register reg) {
		return static_cast<std::uint32_t>(cpu.gpr(reg));
	};
	if (cpu.gpr(Register::rax) == 201 && cpu.gpr(Register::rdi) == 0) {
		cpu.gpr(Register::rax) = 1;
		return true;
	}
	if (!cpu.system_call_32bit)
		return false;
	if ((low(Register::rax) == 265U && low(Register::rbx) == 0U) ||
		(low(Register::rax) == 355U && low(Register::rbx) == 0U &&
		 low(Register::rcx) == 0U && low(Register::rdx) == 0U)) {
		cpu.gpr(Register::rax) = 0;
		return true;
	}
	return false;
}

#if UML_FEX_ASM_JIT
[[nodiscard]] const char *reason_name(uml::cpu::ExitReason reason) noexcept
{
	switch (reason) {
	case uml::cpu::ExitReason::system_call: return "system_call";
	case uml::cpu::ExitReason::memory_fault: return "memory_fault";
	case uml::cpu::ExitReason::interrupt: return "interrupt";
	case uml::cpu::ExitReason::illegal_instruction: return "illegal_instruction";
	case uml::cpu::ExitReason::breakpoint: return "breakpoint";
	case uml::cpu::ExitReason::floating_point_trap: return "floating_point_trap";
	case uml::cpu::ExitReason::pc_alignment: return "pc_alignment";
	case uml::cpu::ExitReason::instruction_budget: return "instruction_budget";
	case uml::cpu::ExitReason::single_step: return "single_step";
	case uml::cpu::ExitReason::fatal: return "fatal";
	case uml::cpu::ExitReason::sme_access: return "sme_access";
	}
	return "unknown";
}

/* --code-size-per-node: the engine's first code-size estimate. A small one
 * makes every large unit outgrow its buffer and restart, which
 * --require-growth then requires to have happened. */
uml::cpu::x86_64::jit::FexEngineOptions jit_options{};
bool require_growth = false;
/* --emitted-registers: check that no block FEXCore emits for the corpus
 * names x18 (fex_emitted_registers.hpp). */
bool check_emitted_registers = false;

/* One engine serves the process, as one serves a vCPU thread. Every case
 * runs in a fresh address-space generation, so no translation, software-TLB
 * entry or code-page registration crosses cases. */
struct JitHarness final {
	uml::cpu::x86_64::jit::FexEngine engine{
		uml::platform::apple::probed_host_capabilities(), jit_options};
	unsigned long generation{};
	uml::cpu::InterruptToken interrupt{};

	/* A case starts in `mode` and may reach the other execution mode
	 * through a far transfer, so the generation gets a space in each. */
	[[nodiscard]] unsigned long begin(uml::cpu::x86_64::ExecutionMode mode,
		std::uint64_t code_sequence)
	{
		using uml::cpu::x86_64::ExecutionMode;
		++generation;
		for (const auto each : {mode, mode == ExecutionMode::long64 ?
			ExecutionMode::compat32 : ExecutionMode::long64})
			engine.apply_changes(generation, each, {
				.mapping_sequence = 1,
				.code_sequence = code_sequence,
				.all_mappings = true,
				.all_code = true,
			});
		return generation;
	}

	[[nodiscard]] uml::cpu::x86_64::jit::JitRunResult run(
		uml::cpu::x86_64::CpuState &cpu, SparseMemory &memory,
		unsigned long run_generation, std::size_t budget)
	{
		return engine.run(cpu, memory, {
			.generation = run_generation,
			.instruction_budget = budget,
			.execution_context = nullptr,
			.interrupt = &interrupt,
		});
	}
};

JitHarness &jit_harness()
{
	static JitHarness harness;
	return harness;
}

constexpr std::uint64_t rflags_trap = 1ULL << 8U;
constexpr std::uint64_t rflags_resume = 1ULL << 16U;
constexpr std::uint32_t vector_general_protection = 13;
#endif
#if UML_FEX_ASM_GADGETS
[[nodiscard]] const char *event_name(uml::cpu::EngineEvent event) noexcept
{
	switch (event) {
	case uml::cpu::EngineEvent::fatal: return "fatal";
	case uml::cpu::EngineEvent::continued: return "continued";
	case uml::cpu::EngineEvent::system_call: return "system_call";
	case uml::cpu::EngineEvent::memory_fault: return "memory_fault";
	case uml::cpu::EngineEvent::interrupt: return "interrupt";
	case uml::cpu::EngineEvent::illegal_instruction: return "illegal_instruction";
	case uml::cpu::EngineEvent::breakpoint: return "breakpoint";
	case uml::cpu::EngineEvent::floating_point_trap:
		return "floating_point_trap";
	case uml::cpu::EngineEvent::pc_alignment: return "pc_alignment";
	case uml::cpu::EngineEvent::overflow: return "overflow";
	case uml::cpu::EngineEvent::general_protection: return "general_protection";
	case uml::cpu::EngineEvent::trap_flag: return "trap_flag";
	case uml::cpu::EngineEvent::debug_trap: return "debug_trap";
	case uml::cpu::EngineEvent::bound_range: return "bound_range";
	case uml::cpu::EngineEvent::sme_access: return "sme_access";
	}
	return "unknown";
}
#endif

#if UML_FEX_ASM_SWITCH
/* --switch-sweep: the product's tiered execution. An address space starts on
 * the gadget engine and, once the gadgets have retired a threshold of
 * instructions, the JIT runs the rest from the state the gadgets left. The
 * runtime hands the state over through the guest registers it saves at the
 * end of every dispatch, which settles the deferred flags; nothing else
 * crosses. A case runs again for each switch point of the sweep until it
 * ends before one, so every instruction boundary of a short case and a
 * geometric ladder of a long one is a handover, and each must end as the
 * gadgets alone and the JIT alone do. */
[[nodiscard]] std::size_t next_switch_point(std::size_t point) noexcept
{
	return point < 8 ? point + 1 : point * 2;
}

[[nodiscard]] uml::cpu::ExitReason exit_reason(uml::cpu::EngineEvent event,
	std::uint32_t &syndrome) noexcept
{
	using uml::cpu::EngineEvent;
	using uml::cpu::ExitReason;
	syndrome = 0;
	switch (event) {
	case EngineEvent::fatal: return ExitReason::fatal;
	case EngineEvent::continued: return ExitReason::instruction_budget;
	case EngineEvent::system_call: return ExitReason::system_call;
	case EngineEvent::memory_fault: return ExitReason::memory_fault;
	case EngineEvent::interrupt: return ExitReason::interrupt;
	case EngineEvent::illegal_instruction:
		return ExitReason::illegal_instruction;
	case EngineEvent::breakpoint:
		syndrome = 3;
		return ExitReason::breakpoint;
	case EngineEvent::floating_point_trap:
		return ExitReason::floating_point_trap;
	case EngineEvent::pc_alignment: return ExitReason::pc_alignment;
	case EngineEvent::overflow:
		syndrome = 4;
		return ExitReason::breakpoint;
	case EngineEvent::general_protection:
		syndrome = vector_general_protection;
		return ExitReason::breakpoint;
	case EngineEvent::trap_flag:
	case EngineEvent::debug_trap: return ExitReason::single_step;
	case EngineEvent::bound_range:
		syndrome = 5;
		return ExitReason::breakpoint;
	case EngineEvent::sme_access: return ExitReason::sme_access;
	}
	return ExitReason::fatal;
}

/* The gadget engine's side of a tiered case, driven as the product's
 * dispatch loop drives it: through the program cache when the case runs on
 * the code-page model, which it must for the JIT to follow. */
class GadgetTier final {
public:
	GadgetTier(SparseMemory &memory, CodeSequences &sequences,
		Dispatch dispatch)
		: memory_{memory}, sequences_{sequences}, dispatch_{dispatch}
	{
		prepare();
	}

	/* The instructions left before the handover, zero once it is due. */
	[[nodiscard]] std::size_t budget() const noexcept
	{
		return switch_after > retired_ ? switch_after - retired_ : 0;
	}

	/* A dispatch of at most `limit` instructions. It ends, as an engine's
	 * does, at a stop or on its budget. */
	[[nodiscard]] uml::cpu::ExecutionResult run(
		uml::cpu::x86_64::CpuState &cpu, std::size_t limit)
	{
		using namespace uml::cpu::x86_64::gadgets;
		uml::cpu::ExecutionResult out{};
		std::size_t done = 0;
		while (done < limit) {
			BuildResult built{};
			const Program *program{};
			if (dispatch_ == Dispatch::cached) {
				if (prepared_code_changes_ != sequences_.code_changes)
					prepare();
				const auto selected = executor_.select_program(memory_,
					cpu.rip, cpu.execution_mode);
				built.status = selected.status;
				program = selected.program;
			} else {
				built = build(memory_, cpu.rip, cpu.execution_mode);
				program = &built.program;
			}
			if (built.status != BuildStatus::built || program == nullptr) {
				using uml::cpu::ExitReason;
				out.reason = built.status == BuildStatus::fetch_fault ?
					ExitReason::memory_fault :
					ExitReason::illegal_instruction;
				break;
			}
			const auto result = execute(*program, cpu, memory_, executor_,
				limit - done);
			done += result.retired_instructions;
			if (dispatch_ == Dispatch::cached) {
				prepared_code_changes_ =
					executor_.prepared_code_changes();
			}
			if (result.event != uml::cpu::EngineEvent::continued) {
				out.reason = exit_reason(result.event, out.syndrome);
				break;
			}
			out.reason = uml::cpu::ExitReason::instruction_budget;
			if (result.instruction_budget_exhausted)
				break;
		}
		out.retired_instructions = done;
		retired_ += done;
		return out;
	}

private:
	void prepare()
	{
		if (dispatch_ == Dispatch::fresh) {
			executor_.prepare(memory_);
			return;
		}
		executor_.prepare(memory_, cached_generation,
			sequences_.mapping_changes, sequences_.code_changes,
			&sequences_, read_code_sequences);
		prepared_code_changes_ = sequences_.code_changes;
	}

	SparseMemory &memory_;
	CodeSequences &sequences_;
	const Dispatch dispatch_;
	uml::cpu::x86_64::gadgets::Executor executor_;
	std::uint64_t prepared_code_changes_{};
	std::size_t retired_{};
};
#endif

} // namespace

[[nodiscard]] int run_configured_case(std::span<const std::byte> binary,
	const Configuration &configuration, std::string_view test,
	bool quiet_success, Dispatch dispatch)
{
	SparseMemory memory;
	CodeSequences sequences;
#if UML_FEX_ASM_JIT
	/* The engine fetches through is_executable: every mapped page may hold
	 * code, as in the product, and a publishing store moves the sequence. */
	(void)dispatch;
	memory.model_code_pages(&sequences.code_changes);
#else
	if (dispatch == Dispatch::cached)
		memory.model_code_pages(&sequences.code_changes);
#endif
	const auto code_size = (binary.size() + page_size - 1U) &
		~(page_size - 1U);
	if (!memory.map(scratch_address, page_size * 10U) ||
		!memory.map(sib_address - page_size, page_size * 2U) ||
		!memory.map(code_address, code_size)) {
		print_result("malformed_input", 0, 0, {}, "invalid base mapping", test);
		return 2;
	}
	for (const auto &region : configuration.memory_regions) {
		if (!memory.map(region.begin, region.end - region.begin)) {
			print_result("malformed_input", 0, region.begin, {},
				"invalid configured mapping", test);
			return 2;
		}
	}
	if (!memory.write(code_address, binary)) {
		print_result("malformed_input", 0, code_address, {},
			"cannot load test code", test);
		return 2;
	}
	for (const auto &data : configuration.memory_data) {
		if (!memory.write(data.address, data.bytes)) {
			print_result("malformed_input", 0, data.address, {},
				"memory data targets an unmapped range", test);
			return 2;
		}
	}

	uml::cpu::x86_64::CpuState cpu{};
#if UML_FEX_ASM_JIT
	cpu.rip = code_address;
	cpu.rflags = 0x202;
	linux_segments(cpu, configuration.mode);
	auto &harness = jit_harness();
	const auto generation = harness.begin(configuration.mode,
		sequences.code_changes);
#if UML_FEX_ASM_SWITCH
	std::optional<GadgetTier> gadgets;
	if (switch_after != 0)
		gadgets.emplace(memory, sequences, dispatch);
#endif
	std::size_t retired{};
	while (retired < instruction_limit) {
		const auto opcode = memory.snapshot(cpu.rip, 15);
		if (const auto finished = finish_at_marker(configuration, cpu, opcode,
			retired, quiet_success, test))
			return *finished;
#if UML_FEX_ASM_SWITCH
		uml::cpu::ExecutionResult dispatched;
		if (gadgets && gadgets->budget() != 0) {
			dispatched = gadgets->run(cpu, std::min(gadgets->budget(),
				instruction_limit - retired));
		} else {
			/* The runtime saves the guest registers at the end of every
			 * dispatch, which settles the deferred flags. */
			uml::cpu::x86_64::materialize_flags(cpu);
			switch_taken = switch_taken || gadgets.has_value();
			dispatched = harness.run(cpu, memory, generation,
				instruction_limit - retired).execution;
		}
		const auto &result = dispatched;
#else
		const auto &result = harness.run(cpu, memory, generation,
			instruction_limit - retired).execution;
#endif
		retired += result.retired_instructions;
		if (result.reason == uml::cpu::ExitReason::instruction_budget) {
			cpu.rflags &= ~rflags_resume;
			continue;
		}
		if (result.reason == uml::cpu::ExitReason::system_call &&
			serve_test_system_call(cpu))
			continue;
		/* HLT, the end marker, raises #GP at user privilege. */
		if (result.reason == uml::cpu::ExitReason::breakpoint &&
			result.syndrome == vector_general_protection) {
			const auto marker = memory.snapshot(cpu.rip, 1);
			if (!marker.empty() && marker.front() == std::byte{0xf4})
				continue;
		}
		const auto status = result.reason ==
			uml::cpu::ExitReason::memory_fault ? "memory_fault" :
			result.reason == uml::cpu::ExitReason::illegal_instruction ?
			"illegal_instruction" : "unexpected_event";
		std::string detail = reason_name(result.reason);
		if (result.reason == uml::cpu::ExitReason::breakpoint)
			detail += " vector " + std::to_string(result.syndrome);
		print_result(status, retired, cpu.rip,
			memory.snapshot(cpu.rip, 15), detail, test);
		reported.rax = cpu.gpr(uml::cpu::x86_64::Register::rax);
		return 1;
	}
#else
	uml::cpu::x86_64::gadgets::Executor executor;
	/* A cached run prepares the executor as the product's dispatch loop
	 * does: again whenever the code-change sequence has moved past the one
	 * it was prepared with. */
	std::uint64_t prepared_code_changes{};
	const auto prepare = [&]() {
		if (dispatch == Dispatch::fresh) {
			executor.prepare(memory);
			return;
		}
		executor.prepare(memory, cached_generation,
			sequences.mapping_changes, sequences.code_changes, &sequences,
			read_code_sequences);
		prepared_code_changes = sequences.code_changes;
	};
	prepare();
	cpu.rip = code_address;
	cpu.rflags = 0x202;
	linux_segments(cpu, configuration.mode);
	std::size_t retired{};
	while (retired < instruction_limit) {
		const auto opcode = memory.snapshot(cpu.rip, 15);
		if (const auto finished = finish_at_marker(configuration, cpu, opcode,
			retired, quiet_success, test))
			return *finished;

		uml::cpu::x86_64::gadgets::BuildResult built{};
		const uml::cpu::x86_64::gadgets::Program *program{};
		if (dispatch == Dispatch::cached) {
			if (prepared_code_changes != sequences.code_changes)
				prepare();
			const auto selected = executor.select_program(
				memory, cpu.rip, cpu.execution_mode);
			built.status = selected.status;
			program = selected.program;
		} else {
			built = uml::cpu::x86_64::gadgets::build(
				memory, cpu.rip, cpu.execution_mode);
			program = &built.program;
		}
		if (built.status != uml::cpu::x86_64::gadgets::BuildStatus::built ||
			program == nullptr) {
			std::string_view status = "illegal_instruction";
			std::string_view detail = "decoder rejected opcode";
			if (built.status ==
				uml::cpu::x86_64::gadgets::BuildStatus::fetch_fault) {
				status = "memory_fault";
				detail = "instruction fetch fault";
			} else if (built.status ==
				uml::cpu::x86_64::gadgets::BuildStatus::allocation_failure) {
				status = "allocation_failure";
				detail = "gadget allocation failed";
			}
			print_result(status, retired, cpu.rip, opcode, detail, test);
			return 1;
		}
		const auto remaining = instruction_limit - retired;
		const auto result = uml::cpu::x86_64::gadgets::execute(
			*program, cpu, memory, executor, remaining);
		retired += result.retired_instructions;
		if (dispatch == Dispatch::cached)
			prepared_code_changes = executor.prepared_code_changes();
		if (result.event == uml::cpu::EngineEvent::system_call &&
			serve_test_system_call(cpu))
			continue;
		/* HLT, the test's end marker, raises #GP at user privilege without
		 * retiring. The loop head recognizes it and compares the state. */
		if (result.event == uml::cpu::EngineEvent::general_protection) {
			const auto marker = memory.snapshot(cpu.rip, 1);
			if (!marker.empty() && marker.front() == std::byte{0xf4})
				continue;
		}
		if (result.event != uml::cpu::EngineEvent::continued) {
			const auto status = result.event ==
				uml::cpu::EngineEvent::memory_fault ?
				"memory_fault" : "unexpected_event";
			print_result(status, retired, cpu.rip,
				memory.snapshot(cpu.rip, 15), event_name(result.event), test);
			reported.rax = cpu.gpr(uml::cpu::x86_64::Register::rax);
			return 1;
		}
		if (result.instruction_budget_exhausted)
			break;
	}
#endif
	print_result("instruction_limit", retired, cpu.rip,
		memory.snapshot(cpu.rip, 15), "instruction retirement limit reached",
		test);
	return 1;
}

[[nodiscard]] int run_fex_case(std::span<const std::byte> binary,
	std::span<const std::byte> config_bytes, std::string_view test,
	bool quiet_success = false, Dispatch dispatch = Dispatch::fresh)
{
	std::string error;
	if (binary.empty()) {
		print_result("malformed_input", 0, 0, {},
			"test binary is empty", test);
		return 2;
	}
	Configuration configuration;
	if (!parse_configuration(config_bytes, configuration, error)) {
		print_result("malformed_input", 0, 0, {}, error, test);
		return 2;
	}
	return run_configured_case(binary, configuration, test, quiet_success,
		dispatch);
}

namespace {

constexpr std::array<std::byte, 8> corpus_magic{
	std::byte{'U'}, std::byte{'M'}, std::byte{'L'}, std::byte{'X'},
	std::byte{'8'}, std::byte{'6'}, std::byte{'T'}, std::byte{0}};
constexpr std::uint32_t corpus_version = 1;
/* FEX programs and repository cases share FEX's final-state configuration;
 * K records are per-instruction traces. */
constexpr std::uint8_t corpus_kind_fex = 1;
constexpr std::uint8_t corpus_kind_k = 2;
constexpr std::uint8_t corpus_kind_repository = 3;
constexpr std::uint64_t k_low_mapping_size = 0x1'0000'0000;

class ByteCursor final {
public:
	explicit ByteCursor(std::span<const std::byte> bytes) noexcept : bytes_{bytes}
	{
	}

	template<std::unsigned_integral Value>
	[[nodiscard]] bool read(Value &value) noexcept
	{
		if (!read_le(bytes_, offset_, value))
			return false;
		offset_ += sizeof(value);
		return true;
	}

	[[nodiscard]] bool take(std::size_t size,
		std::span<const std::byte> &value) noexcept
	{
		if (size > remaining())
			return false;
		value = bytes_.subspan(offset_, size);
		offset_ += size;
		return true;
	}

	[[nodiscard]] std::size_t remaining() const noexcept
	{
		return bytes_.size() - offset_;
	}

private:
	std::span<const std::byte> bytes_;
	std::size_t offset_{};
};

struct KExpectedState final {
	std::uint32_t rip_index{};
	std::uint16_t gpr_valid{};
	std::uint16_t vector_valid{};
	std::uint8_t flag_valid{};
	std::uint8_t flag_values{};
	std::uint8_t expected_event{};
	std::array<std::uint64_t, 16> gprs{};
	std::array<std::array<std::byte, 32>, 16> vectors{};
};

[[nodiscard]] bool read_k_state(ByteCursor &cursor, KExpectedState &state,
	std::string &error)
{
	std::uint16_t gpr_changed{};
	std::uint16_t vector_changed{};
	std::uint8_t flag_changed{};
	if (!cursor.read(state.rip_index) ||
		!cursor.read(state.gpr_valid) ||
		!cursor.read(gpr_changed) ||
		!cursor.read(state.vector_valid) ||
		!cursor.read(vector_changed) ||
		!cursor.read(state.flag_valid) ||
		!cursor.read(flag_changed) ||
		!cursor.read(state.flag_values) ||
		!cursor.read(state.expected_event)) {
		error = "truncated K state header";
		return false;
	}
	if (state.expected_event > 1 || (gpr_changed & ~state.gpr_valid) != 0 ||
		(vector_changed & ~state.vector_valid) != 0 ||
		(flag_changed & ~state.flag_valid) != 0 ||
		(state.flag_valid & 0x80U) != 0) {
		error = "invalid K state masks";
		return false;
	}
	for (unsigned index = 0; index < state.gprs.size(); ++index) {
		if ((gpr_changed & (1U << index)) != 0 &&
			!cursor.read(state.gprs[index])) {
			error = "truncated K general-register state";
			return false;
		}
	}
	for (unsigned index = 0; index < state.vectors.size(); ++index) {
		if ((vector_changed & (1U << index)) == 0)
			continue;
		std::span<const std::byte> bytes;
		if (!cursor.take(state.vectors[index].size(), bytes)) {
			error = "truncated K vector-register state";
			return false;
		}
		std::memcpy(state.vectors[index].data(), bytes.data(), bytes.size());
	}
	return true;
}

[[nodiscard]] std::uint8_t compact_flags(
	const uml::cpu::x86_64::CpuState &cpu) noexcept
{
	std::uint8_t value{};
	for (unsigned index = 0; index < pack_flags.size(); ++index)
		value |= ((cpu.rflags & pack_flags[index]) != 0) << index;
	return value;
}

void initialize_k_cpu(uml::cpu::x86_64::CpuState &cpu,
	const KExpectedState &expected, std::span<const std::uint32_t> offsets)
{
	for (unsigned index = 0; index < pack_registers.size(); ++index) {
		if ((expected.gpr_valid & (1U << index)) != 0)
			cpu.gpr(pack_registers[index]) = expected.gprs[index];
	}
	for (unsigned index = 0; index < cpu.vectors.size(); ++index) {
		if ((expected.vector_valid & (1U << index)) != 0)
			cpu.vectors[index] = expected.vectors[index];
	}
	cpu.rflags = uml::cpu::x86_64::rflags_reserved;
	for (unsigned index = 0; index < pack_flags.size(); ++index) {
		if ((expected.flag_valid & expected.flag_values & (1U << index)) != 0)
			cpu.rflags |= pack_flags[index];
	}
	cpu.rip = code_address + offsets[expected.rip_index];
	linux_segments(cpu, uml::cpu::x86_64::ExecutionMode::long64);
}

[[nodiscard]] bool binary32_nan(std::uint32_t value) noexcept
{
	return (value & 0x7f80'0000U) == 0x7f80'0000U &&
		(value & 0x007f'ffffU) != 0;
}

[[nodiscard]] bool binary64_nan(std::uint64_t value) noexcept
{
	return (value & 0x7ff0'0000'0000'0000ULL) ==
			0x7ff0'0000'0000'0000ULL &&
		(value & 0x000f'ffff'ffff'ffffULL) != 0;
}

[[nodiscard]] bool equivalent_k_vector(
	const std::array<std::byte, 32> &expected,
	const std::array<std::byte, 32> &actual,
	bool nan_class_equivalence) noexcept
{
	if (expected == actual)
		return true;
	if (!nan_class_equivalence)
		return false;
	/* K's floating-point rules often select one concrete NaN payload where
	 * Intel either preserves an input payload or permits several quiet NaNs.
	 * Retain K as a class-level oracle for those lanes; native-oracle and FEX
	 * records remain bit-exact.
	 */
	for (unsigned lane = 0; lane < 4; ++lane) {
		std::uint64_t expected64{};
		std::uint64_t actual64{};
		std::memcpy(&expected64, expected.data() + lane * sizeof(expected64),
			sizeof(expected64));
		std::memcpy(&actual64, actual.data() + lane * sizeof(actual64),
			sizeof(actual64));
		if (expected64 == actual64 ||
			(binary64_nan(expected64) && binary64_nan(actual64)))
			continue;
		bool binary32_lanes_match = true;
		for (unsigned half = 0; half < 2; ++half) {
			std::uint32_t expected32{};
			std::uint32_t actual32{};
			const auto offset = lane * sizeof(expected64) +
				half * sizeof(expected32);
			std::memcpy(&expected32, expected.data() + offset,
				sizeof(expected32));
			std::memcpy(&actual32, actual.data() + offset,
				sizeof(actual32));
			if (expected32 != actual32 &&
				!(binary32_nan(expected32) && binary32_nan(actual32))) {
				binary32_lanes_match = false;
				break;
			}
		}
		if (!binary32_lanes_match)
			return false;
	}
	return true;
}

[[nodiscard]] bool compare_k_state(const uml::cpu::x86_64::CpuState &cpu,
	const KExpectedState &expected, std::span<const std::uint32_t> offsets,
	bool nan_class_equivalence, std::string &detail)
{
	const auto expected_rip = code_address + offsets[expected.rip_index];
	if (cpu.rip != expected_rip) {
		std::ostringstream mismatch;
		mismatch << "RIP expected=0x" << std::hex << expected_rip <<
			" actual=0x" << cpu.rip;
		detail = mismatch.str();
		return false;
	}
	for (unsigned index = 0; index < pack_registers.size(); ++index) {
		if ((expected.gpr_valid & (1U << index)) == 0 ||
			cpu.gpr(pack_registers[index]) == expected.gprs[index])
			continue;
		std::ostringstream mismatch;
		mismatch << register_name(index) << " expected=0x" << std::hex <<
			expected.gprs[index] << " actual=0x" << cpu.gpr(pack_registers[index]);
		detail = mismatch.str();
		return false;
	}
	for (unsigned index = 0; index < cpu.vectors.size(); ++index) {
		if ((expected.vector_valid & (1U << index)) != 0 &&
			!equivalent_k_vector(expected.vectors[index], cpu.vectors[index],
				nan_class_equivalence)) {
			for (unsigned lane = 0; lane < 4; ++lane) {
				std::uint64_t expected_lane{};
				std::uint64_t actual_lane{};
				std::memcpy(&expected_lane,
					expected.vectors[index].data() + lane * sizeof(expected_lane),
					sizeof(expected_lane));
				std::memcpy(&actual_lane,
					cpu.vectors[index].data() + lane * sizeof(actual_lane),
					sizeof(actual_lane));
				if (expected_lane == actual_lane)
					continue;
				std::ostringstream mismatch;
				mismatch << "YMM" << index << '[' << lane <<
					"] expected=0x" << std::hex << expected_lane <<
					" actual=0x" << actual_lane;
				detail = mismatch.str();
				break;
			}
			return false;
		}
	}
	const auto actual_flags = compact_flags(cpu);
	if (((actual_flags ^ expected.flag_values) & expected.flag_valid) != 0) {
		std::ostringstream mismatch;
		mismatch << "RFLAGS expected=0x" << std::hex <<
			static_cast<unsigned>(expected.flag_values) << " actual=0x" <<
			static_cast<unsigned>(actual_flags) << " mask=0x" <<
			static_cast<unsigned>(expected.flag_valid);
		detail = mismatch.str();
		return false;
	}
	return true;
}

[[nodiscard]] int run_k_trace(std::span<const std::byte> binary,
	std::span<const std::byte> payload, std::uint32_t expected_cases,
	std::string_view test, bool nan_class_equivalence)
{
	ByteCursor cursor{payload};
	std::uint32_t instruction_count{};
	std::uint32_t state_count{};
	std::uint32_t offset_count{};
	if (!cursor.read(instruction_count) || !cursor.read(state_count) ||
		!cursor.read(offset_count) || state_count < 2 ||
		offset_count != instruction_count + 1U ||
		expected_cases != state_count - 1U) {
		print_result("malformed_input", 0, 0, {},
			"invalid K trace header", test);
		return 2;
	}
	std::vector<std::uint32_t> offsets(offset_count);
	for (auto &offset : offsets) {
		if (!cursor.read(offset) || offset > binary.size()) {
			print_result("malformed_input", 0, 0, {},
				"invalid K instruction offset", test);
			return 2;
		}
	}
	if (!std::is_sorted(offsets.begin(), offsets.end()) ||
		std::adjacent_find(offsets.begin(), offsets.end()) != offsets.end()) {
		print_result("malformed_input", 0, 0, {},
			"unordered K instruction offsets", test);
		return 2;
	}

	SparseMemory memory;
	if (!memory.map(0, k_low_mapping_size) || !memory.write(code_address, binary)) {
		print_result("malformed_input", 0, code_address, {},
			"cannot map K test memory", test);
		return 2;
	}
	KExpectedState expected{};
	std::string error;
	if (!read_k_state(cursor, expected, error) ||
		expected.rip_index >= offsets.size()) {
		print_result("malformed_input", 0, 0, {}, error, test);
		return 2;
	}
	uml::cpu::x86_64::CpuState cpu{};
	initialize_k_cpu(cpu, expected, offsets);
#if UML_FEX_ASM_JIT
	std::uint64_t k_code_changes{1};
	memory.model_code_pages(&k_code_changes);
	auto &harness = jit_harness();
	const auto generation = harness.begin(cpu.execution_mode, k_code_changes);
#endif
#if UML_FEX_ASM_GADGETS
	uml::cpu::x86_64::gadgets::Executor executor;
	executor.prepare(memory);
#endif
	for (std::uint32_t state_index = 1; state_index < state_count;
		++state_index) {
		const auto address = cpu.rip;
		const auto opcode = memory.snapshot(address, 15);
		if (!read_k_state(cursor, expected, error) ||
			expected.rip_index >= offsets.size()) {
			print_result("malformed_input", state_index, address, opcode,
				error, test);
			return 2;
		}
#if UML_FEX_ASM_SWITCH
		/* The first `switch_after` instructions run on the gadgets. */
		const bool on_jit = switch_after == 0 || state_index > switch_after;
		if (on_jit && switch_after != 0)
			switch_taken = true;
#elif UML_FEX_ASM_JIT
		constexpr bool on_jit = true;
#else
		constexpr bool on_jit = false;
#endif
#if UML_FEX_ASM_JIT
		if (on_jit) {
			/* One instruction per dispatch: the engine's single step. Under TF
			 * a repeated string instruction traps after each iteration, with
			 * RF set and RIP still on it; a K state follows the whole
			 * instruction. */
			auto result = uml::cpu::ExecutionResult{};
			for (std::size_t iteration = 0; iteration < instruction_limit;
				++iteration) {
				cpu.rflags |= rflags_trap;
				result = harness.run(cpu, memory, generation, 1).execution;
				if (result.reason != uml::cpu::ExitReason::single_step ||
					cpu.rip != address || (cpu.rflags & rflags_resume) == 0)
					break;
			}
			cpu.rflags &= ~(rflags_trap | rflags_resume);
			const auto expected_reason = expected.expected_event == 0 ?
				uml::cpu::ExitReason::single_step :
				uml::cpu::ExitReason::floating_point_trap;
			if (result.reason != expected_reason) {
				std::string detail = reason_name(result.reason);
				if (result.reason == uml::cpu::ExitReason::breakpoint)
					detail += " vector " + std::to_string(result.syndrome);
				print_result(result.reason ==
					uml::cpu::ExitReason::illegal_instruction ?
					"illegal_instruction" : "unexpected_event",
					state_index - 1U, address, opcode, detail, test);
				return 1;
			}
		}
#endif
#if UML_FEX_ASM_GADGETS
		if (!on_jit) {
			const auto built = uml::cpu::x86_64::gadgets::build(
				memory, address, cpu.execution_mode);
			if (built.status != uml::cpu::x86_64::gadgets::BuildStatus::built) {
				const auto status = built.status ==
					uml::cpu::x86_64::gadgets::BuildStatus::fetch_fault ?
					"memory_fault" : "illegal_instruction";
				print_result(status, state_index - 1U, address, opcode,
					"K trace instruction did not build", test);
				return 1;
			}
			const auto result = uml::cpu::x86_64::gadgets::execute(
				built.program, cpu, memory, executor, 1);
			const auto expected_event = expected.expected_event == 0 ?
				uml::cpu::EngineEvent::continued :
				uml::cpu::EngineEvent::floating_point_trap;
			const auto expected_retired = expected.expected_event == 0 ? 1U : 0U;
			if (result.event != expected_event ||
				result.retired_instructions != expected_retired) {
				print_result("unexpected_event", state_index - 1U, address, opcode,
					event_name(result.event), test);
				return 1;
			}
		}
#endif
		if (expected.expected_event != 0)
			cpu.rip = code_address + offsets[expected.rip_index];
		uml::cpu::x86_64::materialize_flags(cpu);
		std::string detail;
		if (!compare_k_state(cpu, expected, offsets,
			nan_class_equivalence, detail)) {
			print_result("result_mismatch", state_index, address, opcode,
				detail, test);
			return 1;
		}
	}
	if (cursor.remaining() != 0) {
		print_result("malformed_input", state_count - 1U, cpu.rip, {},
			"trailing K trace data", test);
		return 2;
	}
	return 0;
}

/* An engine's known failure classes. Each row names one record and its
 * class: a failure of the record is expected, and a pass is a stale row,
 * which fails the run until the list loses it. */
using Exclusions = std::map<std::string, std::string, std::less<>>;

[[nodiscard]] bool load_exclusions(const char *path, Exclusions &exclusions,
	std::string &error)
{
	std::ifstream input{path};
	if (!input) {
		error = std::string{"cannot read exclusion list "} + path;
		return false;
	}
	std::string line;
	while (std::getline(input, line)) {
		if (line.empty() || line.front() == '#')
			continue;
		const auto tab = line.find('\t');
		if (tab == std::string::npos || tab == 0 || tab + 1 == line.size() ||
			line.find('\t', tab + 1) != std::string::npos) {
			error = "malformed exclusion: " + line;
			return false;
		}
		if (!exclusions.emplace(line.substr(0, tab),
			    line.substr(tab + 1)).second) {
			error = "duplicate exclusion: " + line.substr(0, tab);
			return false;
		}
	}
	return true;
}

[[nodiscard]] int run_corpus(std::span<const std::byte> bytes,
	const Exclusions &exclusions = {})
{
	ByteCursor cursor{bytes};
	std::span<const std::byte> magic;
	std::uint32_t version{};
	std::uint32_t record_count{};
	if (!cursor.take(corpus_magic.size(), magic) ||
		!std::equal(magic.begin(), magic.end(), corpus_magic.begin()) ||
		!cursor.read(version) || version != corpus_version ||
		!cursor.read(record_count)) {
		print_result("malformed_input", 0, 0, {}, "invalid corpus header");
		return 2;
	}
	std::uint64_t cases{};
	std::uint32_t failures{};
	std::map<std::string_view, std::uint32_t> expected;
	std::set<std::string_view> named;
#if UML_FEX_ASM_JIT
	uml::testing::fex::EmittedRegisterScan emitted_registers;
	if (check_emitted_registers) {
		/* The engine installs FEXCore's executable-memory hooks with its
		 * first context; wrap them before any case compiles. */
		(void)jit_harness().begin(uml::cpu::x86_64::ExecutionMode::long64,
			1);
		uml::testing::fex::EmittedRegisterScan::install();
	}
#endif
	for (std::uint32_t record_index = 0; record_index < record_count;
		++record_index) {
		std::uint8_t kind{};
		std::uint8_t flags{};
		std::uint16_t reserved{};
		std::uint16_t name_size{};
		std::uint16_t source_size{};
		std::uint32_t code_size{};
		std::uint32_t payload_size{};
		std::uint32_t case_count{};
		if (!cursor.read(kind) || !cursor.read(flags) ||
			!cursor.read(reserved) || !cursor.read(name_size) ||
			!cursor.read(source_size) || !cursor.read(code_size) ||
			!cursor.read(payload_size) || !cursor.read(case_count) ||
			reserved != 0 || (flags & ~std::uint8_t{3}) != 0 ||
			kind < corpus_kind_fex || kind > corpus_kind_repository ||
			(kind != corpus_kind_k && (flags & 2U) != 0) ||
			(kind == corpus_kind_repository &&
				(flags != 0 || case_count != 1)) ||
			case_count == 0) {
			print_result("malformed_input", record_index, 0, {},
				"invalid corpus record header");
			return 2;
		}
		std::span<const std::byte> name_bytes;
		std::span<const std::byte> source_bytes;
		std::span<const std::byte> code;
		std::span<const std::byte> payload;
		if (!cursor.take(name_size, name_bytes) ||
			!cursor.take(source_size, source_bytes) ||
			!cursor.take(code_size, code) ||
			!cursor.take(payload_size, payload)) {
			print_result("malformed_input", record_index, 0, {},
				"truncated corpus record");
			return 2;
		}
		const std::string_view name{
			reinterpret_cast<const char *>(name_bytes.data()), name_bytes.size()};
		const auto exclusion = exclusions.find(name);
		if (exclusion != exclusions.end()) {
			excluded_class = exclusion->second;
			named.insert(exclusion->first);
		}
		const auto run_record = [&]() {
			return kind == corpus_kind_k ?
				run_k_trace(code, payload, case_count, name,
					(flags & 2U) != 0) :
				run_fex_case(code, payload, name, true,
					kind == corpus_kind_repository ?
						Dispatch::cached : Dispatch::fresh);
		};
#if UML_FEX_ASM_SWITCH
		int result = 0;
		if (switch_sweep) {
			/* The JIT alone is the JIT corpus test's; every handover from
			 * the gadgets must end the same way. */
			for (switch_after = 1; switch_after <= instruction_limit;
				switch_after = next_switch_point(switch_after)) {
				switch_taken = false;
				result = run_record();
				if (result != 0 || !switch_taken)
					break;
			}
			switch_after = 0;
		} else {
			result = run_record();
		}
#else
		const auto result = run_record();
#endif
		excluded_class = {};
#if UML_FEX_ASM_JIT
		/* Before a later case's space can retire this one's buffers. */
		if (check_emitted_registers)
			emitted_registers.scan();
#endif
		cases += case_count;
		if (exclusion == exclusions.end() || result == 2) {
			failures += result != 0;
		} else if (result == 0) {
			print_result("stale_exclusion", 0, 0, {},
				"passes; remove it from the exclusion list", name);
			++failures;
		} else {
			++expected[exclusion->second];
		}
	}
	for (const auto &[name, failure_class] : exclusions) {
		if (named.contains(name))
			continue;
		print_result("unknown_exclusion", 0, 0, {},
			"names no record of the corpus", name);
		++failures;
	}
	if (cursor.remaining() != 0) {
		print_result("malformed_input", record_count, 0, {},
			"trailing corpus bytes");
		return 2;
	}
#if UML_FEX_ASM_JIT
	const auto restarts = jit_harness().engine.code_buffer_restarts();
	if (require_growth && restarts == 0) {
		print_result("no_growth", 0, 0, {},
			"no compile outgrew its first code buffer");
		++failures;
	}
	if (check_emitted_registers) {
		std::cout << "emitted-registers: " << emitted_registers.summary()
			  << '\n';
		failures += emitted_registers.passed() ? 0U : 1U;
	}
#endif
	std::cout << "status=" << (failures == 0 ? "passed" : "failed") << '\t'
		<< "records=" << record_count << '\t' << "cases=" << cases << '\t'
		<< "failures=" << failures;
#if UML_FEX_ASM_JIT
	std::cout << '\t' << "restarts=" << restarts;
#endif
	if (!exclusions.empty()) {
		std::uint32_t excluded{};
		for (const auto &[failure_class, count] : expected)
			excluded += count;
		std::cout << '\t' << "excluded=" << excluded << '\t' << "classes=";
		const char *separator = "";
		for (const auto &[failure_class, count] : expected) {
			std::cout << separator << failure_class << ':' << count;
			separator = ",";
		}
	}
	std::cout << '\n';
	return failures == 0 ? 0 : 1;
}

/* --retirement: the instructions a run retires before each kind of stop.
 * ExecutionResult.retired_instructions feeds the runtime's budget and timer
 * accounting, so it counts only instructions that completed. Both engines
 * run these cases against the same counts, the gadget engine's: a fault or
 * #UD retires nothing of its instruction, a trap or system call retires it,
 * a repeated string instruction retires once however many elements it moves
 * and a loop retires every pass, across dispatches when it outlasts one. A
 * store into the code of the block running it stops that block too, and a
 * case that names RAX checks that the run went on with the new code. A case
 * that names RAX at its stop checks the register a faulting instruction runs
 * again with, as the instructions before it left it. The compat32 cases
 * hold 32-bit code to the same rules. */
struct RetirementCase final {
	std::string_view name;
	std::vector<std::uint8_t> code;
	std::vector<Region> regions;
	std::string_view status;
	std::size_t retired{};
	std::uint64_t address{};
	std::optional<std::uint64_t> rax{};
	uml::cpu::x86_64::ExecutionMode mode{
		uml::cpu::x86_64::ExecutionMode::long64};
	/* RAX at a stop short of the end: what the instructions before a
	 * faulting one wrote to it, which the instruction runs again with. */
	std::optional<std::uint64_t> stop_rax{};
};

[[nodiscard]] std::vector<RetirementCase> retirement_cases()
{
	constexpr std::uint64_t data = 0x2000'0000;
	/* nop; jmp to the page's last four bytes: nop; nop; and a MOV whose
	 * immediate lies in the unmapped page after. */
	std::vector<std::uint8_t> straddle(page_size, 0);
	for (const auto &[offset, byte] : std::initializer_list<
		     std::pair<std::size_t, std::uint8_t>>{{0, 0x90}, {1, 0xe9},
		     {2, 0xf6}, {3, 0x0f}, {0xffc, 0x90}, {0xffd, 0x90},
		     {0xffe, 0x48}, {0xfff, 0xb8}})
		straddle[offset] = byte;
	/* mov $0x20, %eax; movabs $0x9090909090909090, %rbx; jmp to 0xff6:
	 * mov %rbx, 1f(%rip), a store across the page boundary into the
	 * instruction after it; 1: mov $0x11111111, %eax; nop; nop; nop;
	 * hlt. */
	std::vector<std::uint8_t> crossing(page_size + 6, 0);
	for (const auto &[offset, byte] : std::initializer_list<
		     std::pair<std::size_t, std::uint8_t>>{{0, 0xb8}, {1, 0x20},
		     {5, 0x48}, {6, 0xbb}, {0xf, 0xe9}, {0x10, 0xe2}, {0x11, 0x0f},
		     {0xff6, 0x48}, {0xff7, 0x89}, {0xff8, 0x1d}, {0xffd, 0xb8},
		     {0xffe, 0x11}, {0xfff, 0x11}, {0x1000, 0x11}, {0x1001, 0x11},
		     {0x1002, 0x90}, {0x1003, 0x90}, {0x1004, 0x90},
		     {0x1005, 0xf4}})
		crossing[offset] = byte;
	std::fill(crossing.begin() + 7, crossing.begin() + 0xf, 0x90);
	constexpr auto compat32 = uml::cpu::x86_64::ExecutionMode::compat32;
	constexpr auto long64 = uml::cpu::x86_64::ExecutionMode::long64;
	const auto le32 = [](std::uint64_t value) {
		return std::array<std::uint8_t, 4>{
			static_cast<std::uint8_t>(value),
			static_cast<std::uint8_t>(value >> 8U),
			static_cast<std::uint8_t>(value >> 16U),
			static_cast<std::uint8_t>(value >> 24U)};
	};
	/* movb $0x90, 1f; movb $0x90, 1f+1; 1: inc %eax; hlt, through
	 * absolute addresses. */
	const auto first = le32(code_address + 14);
	const auto second = le32(code_address + 15);
	const std::vector<std::uint8_t> code_write32{0xc6, 0x05, first[0],
		first[1], first[2], first[3], 0x90, 0xc6, 0x05, second[0],
		second[1], second[2], second[3], 0x90, 0xff, 0xc0, 0xf4};
	/* mov $data, %edx; movl $1f, (%edx); movw $0x23, 4(%edx);
	 * mov $data+0x800, %esp; lcall *(%edx); hlt; 1: lret */
	const auto target = le32(code_address + 25);
	const std::vector<std::uint8_t> far_call32{0xba, 0x00, 0x00, 0x00,
		0x20, 0xc7, 0x02, target[0], target[1], target[2], target[3], 0x66,
		0xc7, 0x42, 0x04, 0x23, 0x00, 0xbc, 0x00, 0x08, 0x00, 0x20, 0xff,
		0x1a, 0xf4, 0xcb};
	return {
		/* nop; nop; mov $0x30000000, %rbx; mov (%rbx), %rax; nop; hlt */
		{"load-fault", {0x90, 0x90, 0x48, 0xc7, 0xc3, 0x00, 0x00, 0x00, 0x30,
			0x48, 0x8b, 0x03, 0x90, 0xf4}, {}, "memory_fault", 3,
			code_address + 9},
		/* mov $0x30000000, %ebx; mov $0x1234, %ecx; mov $5, %eax;
		 * mov %rcx, %rax; xchg %rax, (%rbx); hlt: the exchange faults,
		 * and runs again with the RAX the move left. */
		{"xchg-fault-after-move", {0xbb, 0x00, 0x00, 0x00, 0x30, 0xb9, 0x34,
			0x12, 0x00, 0x00, 0xb8, 0x05, 0x00, 0x00, 0x00, 0x48, 0x89, 0xc8,
			0x48, 0x87, 0x03, 0xf4}, {}, "memory_fault", 4, code_address + 18,
			{}, long64, 0x1234},
		/* nop; xor %ecx, %ecx; xor %edx, %edx; div %ecx; nop; hlt */
		{"divide-error", {0x90, 0x31, 0xc9, 0x31, 0xd2, 0xf7, 0xf1, 0x90,
			0xf4}, {}, "unexpected_event", 3, code_address + 5},
		/* nop; nop; ud2; nop; hlt */
		{"ud2", {0x90, 0x90, 0x0f, 0x0b, 0x90, 0xf4}, {},
			"illegal_instruction", 2, code_address + 2},
		/* nop; nop; an opcode invalid in 64-bit mode (PUSH ES); nop; hlt */
		{"invalid-opcode", {0x90, 0x90, 0x06, 0x90, 0xf4}, {},
			"illegal_instruction", 2, code_address + 2},
		/* nop; nop; int3; nop; hlt */
		{"int3", {0x90, 0x90, 0xcc, 0x90, 0xf4}, {}, "unexpected_event", 3,
			code_address + 3},
		/* nop; nop; int $4; nop; hlt */
		{"int-4", {0x90, 0x90, 0xcd, 0x04, 0x90, 0xf4}, {},
			"unexpected_event", 3, code_address + 4},
		/* nop; nop; int $0x21, a #GP; nop; hlt */
		{"int-gp", {0x90, 0x90, 0xcd, 0x21, 0x90, 0xf4}, {},
			"unexpected_event", 2, code_address + 2},
		/* nop; mov $201, %eax; xor %edi, %edi; syscall; nop; hlt */
		{"syscall", {0x90, 0xb8, 0xc9, 0x00, 0x00, 0x00, 0x31, 0xff, 0x0f,
			0x05, 0x90, 0xf4}, {}, "passed", 5, code_address + 11},
		/* nop; mov $265, %eax; xor %ebx, %ebx; int $0x80; nop; hlt */
		{"int-0x80", {0x90, 0xb8, 0x09, 0x01, 0x00, 0x00, 0x31, 0xdb, 0xcd,
			0x80, 0x90, 0xf4}, {}, "passed", 5, code_address + 11},
		/* inc %eax; jmp 1f; nop; 1: inc %eax; jmp 2f; nop; 2: inc %eax;
		 * hlt */
		{"jumps", {0xff, 0xc0, 0xeb, 0x01, 0x90, 0xff, 0xc0, 0xeb, 0x01,
			0x90, 0xff, 0xc0, 0xf4}, {}, "passed", 5, code_address + 12},
		/* mov $10, %ecx; 1: nop; dec %ecx; jnz 1b; hlt */
		{"loop", {0xb9, 0x0a, 0x00, 0x00, 0x00, 0x90, 0xff, 0xc9, 0x75,
			0xfb, 0xf4}, {}, "passed", 31, code_address + 10},
		/* The same for 200000 passes, more than one dispatch's budget. */
		{"long-loop", {0xb9, 0x40, 0x0d, 0x03, 0x00, 0x90, 0xff, 0xc9, 0x75,
			0xfb, 0xf4}, {}, "passed", 600'001, code_address + 10},
		/* mov $data, %rdi; mov $data+0x1000, %rsi; mov $50, %ecx; cld;
		 * repe cmpsb; nop; hlt */
		{"repe-cmpsb", {0x48, 0xc7, 0xc7, 0x00, 0x00, 0x00, 0x20, 0x48,
			0xc7, 0xc6, 0x00, 0x10, 0x00, 0x20, 0xb9, 0x32, 0x00, 0x00, 0x00,
			0xfc, 0xf3, 0xa6, 0x90, 0xf4}, {{data, data + 0x2000}},
			"passed", 6, code_address + 23},
		/* mov $data, %rdi; mov $0x100000, %ecx; xor %eax, %eax; cld;
		 * rep stosb; nop; hlt */
		{"long-rep-stosb", {0x48, 0xc7, 0xc7, 0x00, 0x00, 0x00, 0x20, 0xb9,
			0x00, 0x00, 0x10, 0x00, 0x31, 0xc0, 0xfc, 0xf3, 0xaa, 0x90, 0xf4},
			{{data, data + 0x10'0000}}, "passed", 6, code_address + 18},
		/* mov $data+0xff0, %rdi; mov $100, %ecx; xor %eax, %eax; cld;
		 * rep stosb, which runs off its page; nop; hlt */
		{"rep-stosb-fault", {0x48, 0xc7, 0xc7, 0xf0, 0x0f, 0x00, 0x20, 0xb9,
			0x64, 0x00, 0x00, 0x00, 0x31, 0xc0, 0xfc, 0xf3, 0xaa, 0x90, 0xf4},
			{{data, data + 0x1000}}, "memory_fault", 4, code_address + 15},
		{"fetch-fault", std::move(straddle), {}, "memory_fault", 4,
			code_address + 0xffe},
		/* movb $0x90, 1f(%rip); movb $0x90, 1f+1(%rip); 1: inc %eax; hlt:
		 * each store rewrites the block it runs in. */
		{"code-write", {0xc6, 0x05, 0x07, 0x00, 0x00, 0x00, 0x90, 0xc6, 0x05,
			0x01, 0x00, 0x00, 0x00, 0x90, 0xff, 0xc0, 0xf4}, {}, "passed", 4,
			code_address + 16, 0},
		/* lea 1f(%rip), %rdi; mov $0x90909090, %eax; mov $10, %ecx; cld;
		 * rep stosb; 1: movabs $0xfabcfabcfabc0123, %rax; hlt */
		{"code-write-rep-stosb", {0x48, 0x8d, 0x3d, 0x0d, 0x00, 0x00, 0x00,
			0xb8, 0x90, 0x90, 0x90, 0x90, 0xb9, 0x0a, 0x00, 0x00, 0x00, 0xfc,
			0xf3, 0xaa, 0x48, 0xb8, 0x23, 0x01, 0xbc, 0xfa, 0xbc, 0xfa, 0xbc,
			0xfa, 0xf4}, {}, "passed", 15, code_address + 30, 0x90909090},
		/* lock xorb $2, 1f+1(%rip); 1: mov $0x30, %al; hlt, after
		 * mov $0x20, %eax */
		{"code-write-locked", {0xb8, 0x20, 0x00, 0x00, 0x00, 0xf0, 0x80,
			0x35, 0x01, 0x00, 0x00, 0x00, 0x02, 0xb0, 0x30, 0xf4}, {},
			"passed", 3, code_address + 15, 0x32},
		/* mov $2, %ecx; jmp 1f; 1: mov $1, %eax; movb $2, 1b+1(%rip);
		 * dec %ecx; jnz 1b; hlt: a loop that rewrites its own first
		 * instruction and runs it again. */
		{"code-write-loop", {0xb9, 0x02, 0x00, 0x00, 0x00, 0xeb, 0x00, 0xb8,
			0x01, 0x00, 0x00, 0x00, 0xc6, 0x05, 0xf5, 0xff, 0xff, 0xff, 0x02,
			0xff, 0xc9, 0x75, 0xf0, 0xf4}, {}, "passed", 10,
			code_address + 23, 2},
		{"code-write-across-pages", std::move(crossing), {}, "passed", 12,
			code_address + 0x1005, 0x20},
		/* mov $data+0xc, %rdi; mov $5, %eax; lock xadd %rax, (%rdi);
		 * lock xadd %rax, (%rdi); hlt: locked operands across a 16-byte
		 * granule. */
		{"split-locked", {0x48, 0xc7, 0xc7, 0x0c, 0x00, 0x00, 0x20, 0xb8,
			0x05, 0x00, 0x00, 0x00, 0xf0, 0x48, 0x0f, 0xc1, 0x07, 0xf0, 0x48,
			0x0f, 0xc1, 0x07, 0xf4}, {{data, data + 0x1000}}, "passed", 4,
			code_address + 22, 5},
		/* mov $data+0xffc, %rdi; nop; lock add %rax, (%rdi), whose
		 * operand runs off its page; nop; hlt */
		{"split-locked-fault", {0x48, 0xc7, 0xc7, 0xfc, 0x0f, 0x00, 0x20,
			0x90, 0xf0, 0x48, 0x01, 0x07, 0x90, 0xf4}, {{data, data + 0x1000}},
			"memory_fault", 2, code_address + 8},
		/* mov $0x32, %eax; mov $0x11111123, %ebx; 8 x nop;
		 * lock xor %rbx, 1f+1(%rip); 1: mov $0x11111111, %eax; hlt: a
		 * locked operand across a granule rewrites the block it runs in. */
		{"code-write-split-locked", {0xb8, 0x32, 0x00, 0x00, 0x00, 0xbb, 0x23,
			0x11, 0x11, 0x11, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
			0xf0, 0x48, 0x31, 0x1d, 0x01, 0x00, 0x00, 0x00, 0xb8, 0x11, 0x11,
			0x11, 0x11, 0xf4}, {}, "passed", 12, code_address + 31, 0x32},
		/* mov $0x20, %eax; stc; adcb $0, 1f+1(%rip); 1: mov $0x30, %al;
		 * hlt: a read-modify-write into the block it runs in, whose
		 * carry in must survive the unit's end before it. */
		{"code-write-adc", {0xb8, 0x20, 0x00, 0x00, 0x00, 0xf9, 0x80, 0x15,
			0x01, 0x00, 0x00, 0x00, 0x00, 0xb0, 0x30, 0xf4}, {}, "passed", 4,
			code_address + 15, 0x31},
		/* mov $data, %rdi; movl $0x2000, 8(%rdi); vmovdqu (%rdi), %xmm2;
		 * vpcmpeqd %xmm1, %xmm1, %xmm1; vpgatherqq %xmm1,
		 * (%rdi,%xmm2,1), %xmm0, whose second element is off the data;
		 * nop; hlt */
		{"gather-fault", {0x48, 0xc7, 0xc7, 0x00, 0x00, 0x00, 0x20, 0xc7, 0x47,
			0x08, 0x00, 0x20, 0x00, 0x00, 0xc5, 0xfa, 0x6f, 0x17, 0xc5, 0xf1,
			0x76, 0xc9, 0xc4, 0xe2, 0xf1, 0x91, 0x04, 0x17, 0x90, 0xf4},
			{{data, data + 0x1000}}, "memory_fault", 4, code_address + 22},
		/* nop; nop; mov $0x30000000, %ebx; mov (%ebx), %eax; nop; hlt */
		{"compat32-load-fault", {0x90, 0x90, 0xbb, 0x00, 0x00, 0x00, 0x30,
			0x8b, 0x03, 0x90, 0xf4}, {}, "memory_fault", 3, code_address + 7,
			{}, compat32},
		/* mov $0x30000000, %ebx; mov $0x1234, %ecx; mov $5, %eax;
		 * mov %ecx, %eax; xchg %eax, (%ebx); hlt: glibc's
		 * __malloc_fork_unlock_parent, whose exchange faults after a
		 * fork. */
		{"compat32-xchg-fault-after-move", {0xbb, 0x00, 0x00, 0x00, 0x30,
			0xb9, 0x34, 0x12, 0x00, 0x00, 0xb8, 0x05, 0x00, 0x00, 0x00, 0x89,
			0xc8, 0x87, 0x03, 0xf4}, {}, "memory_fault", 4, code_address + 17,
			{}, compat32, 0x1234},
		/* nop; nop; ud2; nop; hlt */
		{"compat32-ud2", {0x90, 0x90, 0x0f, 0x0b, 0x90, 0xf4}, {},
			"illegal_instruction", 2, code_address + 2, {}, compat32},
		/* nop; nop; SYSCALL, undefined outside 64-bit mode; nop; hlt */
		{"compat32-syscall", {0x90, 0x90, 0x0f, 0x05, 0x90, 0xf4}, {},
			"illegal_instruction", 2, code_address + 2, {}, compat32},
		/* nop; nop; SYSENTER; nop; hlt */
		{"compat32-sysenter", {0x90, 0x90, 0x0f, 0x34, 0x90, 0xf4}, {},
			"illegal_instruction", 2, code_address + 2, {}, compat32},
		/* nop; nop; les (%edx), %eax, with EDX naming nothing mapped;
		 * nop; hlt */
		{"compat32-les-fault", {0x90, 0x90, 0xc4, 0x02, 0x90, 0xf4}, {},
			"memory_fault", 2, code_address + 2, {}, compat32},
		/* mov $data, %edx; movl $0x12345678, (%edx); movw $0x2b, 4(%edx);
		 * les (%edx), %eax; hlt */
		{"compat32-les", {0xba, 0x00, 0x00, 0x00, 0x20, 0xc7, 0x02, 0x78,
			0x56, 0x34, 0x12, 0x66, 0xc7, 0x42, 0x04, 0x2b, 0x00, 0xc4, 0x02,
			0xf4}, {{data, data + 0x1000}}, "passed", 4, code_address + 19,
			{}, compat32},
		/* nop; mov $265, %eax; xor %ebx, %ebx; int $0x80; nop; hlt */
		{"compat32-int-0x80", {0x90, 0xb8, 0x09, 0x01, 0x00, 0x00, 0x31,
			0xdb, 0xcd, 0x80, 0x90, 0xf4}, {}, "passed", 5, code_address + 11,
			{}, compat32},
		/* mov $0x7fffffff, %eax; add $1, %eax; into; nop; hlt */
		{"compat32-into", {0xb8, 0xff, 0xff, 0xff, 0x7f, 0x83, 0xc0, 0x01,
			0xce, 0x90, 0xf4}, {}, "unexpected_event", 3, code_address + 9,
			{}, compat32},
		/* mov $data, %edx; mov $7, %eax; bound %eax, (%edx), whose bounds
		 * are 0 and 0; nop; hlt */
		{"compat32-bound", {0xba, 0x00, 0x00, 0x00, 0x20, 0xb8, 0x07, 0x00,
			0x00, 0x00, 0x62, 0x02, 0x90, 0xf4}, {{data, data + 0x1000}},
			"unexpected_event", 2, code_address + 10, {}, compat32},
		/* mov $data+0x10, %esp; pusha, whose stores run below the data;
		 * nop; hlt */
		{"compat32-pusha-fault", {0xbc, 0x10, 0x00, 0x00, 0x20, 0x60, 0x90,
			0xf4}, {{data, data + 0x1000}}, "memory_fault", 1,
			code_address + 5, {}, compat32},
		/* mov $10, %ecx; 1: nop; dec %ecx; jnz 1b; hlt: DEC is one byte. */
		{"compat32-loop", {0xb9, 0x0a, 0x00, 0x00, 0x00, 0x90, 0x49, 0x75,
			0xfc, 0xf4}, {}, "passed", 31, code_address + 9, {}, compat32},
		/* mov $data+0xff0, %edi; mov $100, %ecx; xor %eax, %eax; cld;
		 * rep stosb, which runs off its page; nop; hlt */
		{"compat32-rep-stosb-fault", {0xbf, 0xf0, 0x0f, 0x00, 0x20, 0xb9,
			0x64, 0x00, 0x00, 0x00, 0x31, 0xc0, 0xfc, 0xf3, 0xaa, 0x90, 0xf4},
			{{data, data + 0x1000}}, "memory_fault", 4, code_address + 13, {},
			compat32},
		{"compat32-far-call", far_call32, {{data, data + 0x1000}}, "passed",
			6, code_address + 24, {}, compat32},
		{"compat32-code-write", code_write32, {}, "passed", 4,
			code_address + 16, 0, compat32},
	};
}

[[nodiscard]] int run_retirement_cases()
{
	std::uint32_t failures{};
	const auto cases = retirement_cases();
	for (const auto &test : cases) {
		std::vector<std::byte> code(test.code.size());
		std::transform(test.code.begin(), test.code.end(), code.begin(),
			[](std::uint8_t byte) { return std::byte{byte}; });
		Configuration configuration;
		configuration.memory_regions = test.regions;
		configuration.mode = test.mode;
		if (test.rax) {
			/* Expectation keys: bit 0 names RIP, bit 1 RAX. */
			configuration.registers.push_back({1U << 1U, {*test.rax}});
		}
		capture_results = true;
		reported = {};
		(void)run_configured_case(code, configuration, test.name, false,
			Dispatch::cached);
		capture_results = false;
		bool passed = reported.status == test.status &&
			reported.retired == test.retired &&
			reported.address == test.address &&
			(!test.stop_rax || reported.rax == *test.stop_rax);
#if UML_FEX_ASM_SWITCH
		/* The same stop, after the same count, from every handover. */
		for (switch_after = 1; switch_sweep && passed &&
			switch_after <= instruction_limit;
			switch_after = next_switch_point(switch_after)) {
			switch_taken = false;
			capture_results = true;
			reported = {};
			(void)run_configured_case(code, configuration, test.name, false,
				Dispatch::cached);
			capture_results = false;
			passed = reported.status == test.status &&
				reported.retired == test.retired &&
				reported.address == test.address &&
				(!test.stop_rax || reported.rax == *test.stop_rax);
			if (!passed)
				std::cout << "status=failed\ttest=" << test.name <<
					"\tswitch=" << switch_after << "\tstop=" <<
					reported.status << "\tretired=" << reported.retired <<
					'\n';
			if (!switch_taken)
				break;
		}
		switch_after = 0;
#endif
		if (!passed)
			++failures;
		std::cout << "status=" << (passed ? "passed" : "failed") << '\t'
			<< "test=" << test.name << '\t'
			<< "stop=" << reported.status << '\t'
			<< "retired=" << reported.retired << '\t'
			<< "address=0x" << std::hex << reported.address << std::dec;
		if (test.stop_rax)
			std::cout << '\t' << "rax=0x" << std::hex << reported.rax <<
				std::dec;
		if (!passed) {
			std::cout << '\t' << "expected=" << test.status << ',' <<
				test.retired << ",0x" << std::hex << test.address << std::dec;
			if (test.stop_rax)
				std::cout << ",rax=0x" << std::hex << *test.stop_rax <<
					std::dec;
		}
		std::cout << '\n';
	}
	std::cout << "status=" << (failures == 0 ? "passed" : "failed") << '\t'
		<< "cases=" << cases.size() << '\t' << "failures=" << failures << '\n';
	return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc == 2 && std::string_view{argv[1]} == "--retirement")
		return run_retirement_cases();
#if UML_FEX_ASM_SWITCH
	if (argc == 3 && std::string_view{argv[1]} == "--switch-sweep" &&
		std::string_view{argv[2]} == "--retirement") {
		switch_sweep = true;
		return run_retirement_cases();
	}
#endif
	std::vector<std::byte> first;
	std::string error;
	/* Corpus options come before the corpus. */
	const char *exclusion_list = nullptr;
	bool corpus_options = false;
	int index = 1;
	while (index < argc - 1) {
		const std::string_view option{argv[index]};
		if (option == "--exclusions") {
			exclusion_list = argv[index + 1];
			index += 2;
#if UML_FEX_ASM_JIT
		} else if (option == "--code-size-per-node") {
			const std::string_view value{argv[index + 1]};
			auto &per_node = jit_options.code_bytes_per_node;
			const auto parsed = std::from_chars(value.data(),
				value.data() + value.size(), per_node);
			if (parsed.ec != std::errc{} ||
				parsed.ptr != value.data() + value.size() || per_node == 0) {
				print_result("malformed_input", 0, 0, {},
					"--code-size-per-node needs a positive byte count");
				return 2;
			}
			index += 2;
		} else if (option == "--require-growth") {
			require_growth = true;
			++index;
		} else if (option == "--emitted-registers") {
			check_emitted_registers = true;
			++index;
#endif
#if UML_FEX_ASM_SWITCH
		} else if (option == "--switch-sweep") {
			switch_sweep = true;
			++index;
#endif
		} else {
			break;
		}
		corpus_options = true;
	}
	if (index == argc - 1 && (argc == 2 || corpus_options)) {
		Exclusions exclusions;
		if ((exclusion_list != nullptr &&
			    !load_exclusions(exclusion_list, exclusions, error)) ||
			!load_file(argv[index], first, error)) {
			print_result("malformed_input", 0, 0, {}, error);
			return 2;
		}
		return run_corpus(first, exclusions);
	}
	const bool repository = argc == 4 &&
		std::string_view{argv[1]} == "--repository";
	if (argc == 3 || repository) {
		const auto *binary = argv[argc - 2];
		const auto *config = argv[argc - 1];
		std::vector<std::byte> second;
		if (!load_file(binary, first, error) ||
			!load_file(config, second, error)) {
			print_result("malformed_input", 0, 0, {}, error);
			return 2;
		}
		return run_fex_case(first, second, binary, false,
			repository ? Dispatch::cached : Dispatch::fresh);
	}
	print_result("malformed_input", 0, 0, {},
#if UML_FEX_ASM_JIT
		"usage: runner [--exclusions LIST] [--code-size-per-node BYTES] "
		"[--require-growth] [--emitted-registers] [--switch-sweep] CORPUS | "
#else
		"usage: runner [--exclusions LIST] CORPUS | "
#endif
		"runner [--repository] BINARY CONFIG | runner --retirement");
	return 2;
}
