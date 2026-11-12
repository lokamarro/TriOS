// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_immediate_memory_test_support.hpp"
#include "arm64_base_load_store_register_test_support.hpp"
#include "arm64_simd_ordinary_memory_test_support.hpp"

#include <array>
#include <cstdio>
#include <vector>

namespace e = uml::emulator::tests;
namespace t = e::base_immediate_memory;
namespace a = uml::cpu::arm64;

namespace {
void register_memory(e::EngineHarness &harness)
{
	namespace r = arm64_load_store_register_test;
	std::size_t cases{};
	for (const auto &variant : r::integer_variants)
	for (auto option : r::legal_options)
	for (bool scaled : {false, true})
	for (unsigned location = 0; location < 3; ++location)
	for (unsigned role = 0; role < 7; ++role)
	for (bool warm : {false, true}) {
		if (location == 2 && variant.size == 1) continue;
		// The last two roles cover base/index and all-three-input masks.
		if (role >= 5 && (scaled || location != 0 || option != 3)) continue;
		const unsigned rn = 2, rm = role >= 5 ? 2U : role == 3 ? 3U : 4U;
		const unsigned rt = role == 4 || role == 6 ? 2U : 3U;
		const unsigned producer = role == 1 ? rm : role == 2 || role == 3 ? rt : rn;
		const auto offset = location == 0 ? 128U : r::page_size - 1;
		const bool fault = location == 2;
		std::vector<std::byte> bytes((fault ? 1 : 2) * r::page_size);
		r::fill_memory(bytes, static_cast<unsigned>(cases));
		auto expected_bytes = bytes;
		auto initial = r::seeded_state(0x935d'0000U + cases);
		initial.registers[rt] = t::store_value;
		initial.registers[rm] = r::matrix_raw(option, rm);
		initial.registers[rn] = role >= 5 ? (r::data_base + offset) / 2 :
			r::data_base + offset - r::extend_offset(initial.registers[rm], option,
				scaled ? variant.size_code : 0);
		initial.registers[20] = r::data_base + 16;
		initial.registers[producer] -= 16;
		auto expected = initial;
		std::vector<e::ProgramWord> code;
		const auto emit = [&](std::uint32_t word) { code.push_back({code.size() * 4, word}); };
		if (warm) {
			emit(0xf940'000aU | (20U << 5));
			emit(0xf900'000aU | (20U << 5));
			expected.registers[10] = r::decode_little_endian(bytes, 16, 8);
		}
		emit(0x9100'4000U | (producer << 5) | producer);
		expected.registers[producer] += 16;
		r::require(r::address(expected, variant, option, scaled, rm, rn) ==
			r::data_base + offset, "forwarded register test address");
		const auto memory_offset = code.size() * 4;
		emit(r::encode(variant.size_code, variant.operation, rm, option, scaled, 2, rn, rt));
		emit(0x9100'0405U | (rt << 5));
		emit(t::terminal_system_call);
		if (!fault) {
			if (variant.load)
				expected.registers[rt] = r::expected_load(variant,
					r::decode_little_endian(bytes, offset, variant.size));
			else
				r::encode_little_endian(expected_bytes, offset, variant.size, expected.registers[rt]);
			expected.registers[5] = expected.registers[rt] + 1;
		}
		expected.pc = e::engine_test_code_base + (fault ? memory_offset : code.size() * 4);
		const std::array regions{e::MemoryRegion{
			.address = r::data_base, .initial = bytes, .expected = expected_bytes}};
		char name[120];
		std::snprintf(name, sizeof(name), "forward register %.*s option=%u scaled=%u location=%u role=%u warm=%u",
			static_cast<int>(variant.name.size()), variant.name.data(), option, scaled, location, role, warm);
		harness.check_program({.name = name, .code = code, .initial = initial, .expected = expected,
			.event = fault ? e::ExpectedEvent::memory_fault : e::ExpectedEvent::system_call,
			.native = e::NativeRequirements::baseline(),
			.fault = {.address = r::data_base + r::page_size,
				.access = variant.load ? a::MemoryAccess::read : a::MemoryAccess::write},
			.compare_fault = fault, .memory = regions,
			.expected_instructions = fault ? (warm ? 3U : 1U) : code.size()});
		++cases;
	}
	std::printf("scalar register-memory forwarding: %zu cases\n", cases);
}

void vector_addresses(e::EngineHarness &harness)
{
	namespace v = arm64_simd_ordinary_memory_test;
	std::size_t cases{};
	for (const auto &variant : v::single_variants)
	for (auto option : {2U, 3U, 6U, 7U})
	for (bool scaled : {false, true})
	for (unsigned location = 0; location < 3; ++location)
	for (unsigned producer : {2U, 4U}) {
		if (location == 2 && variant.size == 1) continue;
		const bool fault = location == 2;
		const auto offset = location == 0 ? 128U : v::page_size - 1;
		std::vector<std::byte> bytes((fault ? 1 : 2) * v::page_size);
		v::fill_memory(bytes, cases);
		auto expected_bytes = bytes;
		auto initial = v::seeded_state(0x2183'0000U + cases);
		initial.registers[4] = arm64_load_store_register_test::matrix_raw(option, 4);
		initial.registers[2] = v::data_base + offset - v::extend_offset(initial.registers[4],
			option, scaled ? (variant.size == 16 ? 4U : variant.size_code) : 0);
		initial.registers[producer] -= 16;
		auto expected = initial;
		expected.registers[producer] += 16;
		const std::array code{
			e::ProgramWord{0, 0x9100'4000U | (producer << 5) | producer},
			e::ProgramWord{4, v::register_instruction(variant.size_code, variant.operation, 4, option, scaled, 2, 3)},
			e::ProgramWord{8, 0x9100'0445U}, // memory must end the scalar forwarding chain
			e::ProgramWord{12, t::terminal_system_call}};
		if (!fault) {
			v::apply_single_oracle(initial, expected, bytes, expected_bytes, offset, variant, 3);
			expected.registers[5] = expected.registers[2] + 1;
		}
		expected.pc = e::engine_test_code_base + (fault ? 4 : 16);
		const std::array regions{e::MemoryRegion{
			.address = v::data_base, .initial = bytes, .expected = expected_bytes}};
		char name[120];
		std::snprintf(name, sizeof(name), "forward SIMD address %.*s option=%u scaled=%u location=%u producer=%u",
			static_cast<int>(variant.name.size()), variant.name.data(), option, scaled, location, producer);
		harness.check_program({.name = name, .code = code, .initial = initial, .expected = expected,
			.event = fault ? e::ExpectedEvent::memory_fault : e::ExpectedEvent::system_call,
			.native = v::single_native(),
			.fault = {.address = v::data_base + v::page_size,
				.access = variant.load ? a::MemoryAccess::read : a::MemoryAccess::write},
			.compare_fault = fault, .memory = regions,
			.expected_instructions = fault ? 1U : code.size()});
		++cases;
	}
	std::printf("SIMD scalar-address forwarding: %zu cases\n", cases);
}
} // namespace

/* Exercise the shared memory templates through the ordinary engine harness:
 * real MMU mappings, cold/hot TLBs, cross-page helpers, and precise faults.
 * Expected state and bytes come from the independent memory-family oracle. */
void run_gadget_dependency_memory_tests()
{
	e::EngineHarness harness;
	std::size_t cases{};
	for (const auto &variant : t::variants)
	for (auto form : t::all_forms)
	for (unsigned location = 0; location < 3; ++location)
	for (unsigned role = 0; role < 4; ++role)
	for (bool warm : {false, true}) {
		const bool fault = location == 2 && variant.size > 1;
		if (location == 2 && !fault) continue;
		const auto target = t::data_base +
			(location == 0 ? 128 : t::page_size - 1);
		const unsigned rn = role == 3 ? 31U : 2U;
		const unsigned rt = role == 2 ? 31U : 3U;
		const unsigned producer = role == 1 ? rt : rn;
		// SP is checked before address formation; use an aligned SP and a
		// suitable immediate, except post-index which addresses SP directly.
		if (rn == 31U && location != 0) continue;
		const unsigned immediate = form == t::Form::unsigned_offset ? 0U : 16U;
		const auto base = t::base_for_address(variant, form, target, immediate);
		if (rn == 31U && (base & 15U)) continue;
		std::vector<std::byte> bytes((fault ? 1 : 2) * t::page_size);
		t::fill_memory(bytes, static_cast<unsigned>(cases));
		auto expected_bytes = bytes;
		auto initial = t::seeded_state(0x7ead'0000U + cases);
		initial.registers[20] = t::data_base + 16;
		initial.registers[3] = t::store_value;
		if (rn == 31U) initial.sp = base;
		else initial.registers[rn] = base;
		if (producer == 31U) initial.sp -= 16;
		else initial.registers[producer] -= 16;
		auto expected = initial;
		std::vector<e::ProgramWord> code;
		const auto emit = [&](std::uint32_t word) { code.push_back({code.size() * 4, word}); };
		if (warm) {
			emit(0xf940'000aU | (20U << 5)); // ldr x10, [x20]
			emit(0xf900'000aU | (20U << 5)); // str x10, [x20]
			expected.registers[10] = t::decode_little_endian(bytes, 16, 8);
		}
		emit(0x9100'4000U | (producer << 5) | producer);
		if (producer == 31U) expected.sp += 16;
		else expected.registers[producer] += 16;
		const auto memory_offset = code.size() * 4;
		emit(t::encode(variant, form, immediate, rn, rt));
		emit(0x9100'0405U | ((rt == 31 ? 5U : rt) << 5));
		emit(t::terminal_system_call);
		if (!fault) {
			t::apply_oracle(variant, form, immediate, rn, rt,
				expected, expected_bytes, t::data_base);
			expected.registers[5] = expected.registers[rt == 31 ? 5U : rt] + 1;
		}
		expected.pc = e::engine_test_code_base + (fault ? memory_offset : code.size() * 4);
		const std::array regions{e::MemoryRegion{
			.address = t::data_base, .initial = bytes, .expected = expected_bytes}};
		char name[100];
		std::snprintf(name, sizeof(name), "forward %.*s form=%u location=%u role=%u warm=%u",
			static_cast<int>(variant.name.size()), variant.name.data(),
			static_cast<unsigned>(form), location, role, warm);
		harness.check_program({
			.name = name, .code = code, .initial = initial, .expected = expected,
			.event = fault ? e::ExpectedEvent::memory_fault : e::ExpectedEvent::system_call,
			.native = e::NativeRequirements::baseline(),
			.fault = {.address = t::data_base + t::page_size,
				.access = variant.load ? a::MemoryAccess::read : a::MemoryAccess::write},
			.compare_fault = fault, .memory = regions,
			.expected_instructions = fault ? (warm ? 3U : 1U) : code.size(),
		});
		++cases;
	}
	std::printf("scalar memory forwarding: %zu cold/hot/helper/fault cases\n", cases);
	register_memory(harness);
	vector_addresses(harness);
	harness.print_summary("gadget scalar memory forwarding");
}
