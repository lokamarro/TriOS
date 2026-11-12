// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/jit_memory/executable_memory.hpp"
#include "require.hpp"

#include <sys/mman.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/* The executable-memory owner serves JIT memory from one window in this
 * image's 4 GiB block, so generated code calls into the image at the cheap
 * branch-prediction distance. */
namespace {

namespace jit_memory = ::uml::cpu::jit_memory;
using uml::testing::require;

constexpr std::size_t mib = std::size_t{1024} * 1024U;
constexpr std::size_t buffer_bytes = 16U * mib;
constexpr std::uintptr_t block_bytes = std::uintptr_t{1} << 32U;

[[nodiscard]] bool near_image(const void *mapping, std::size_t size)
{
	const auto image = reinterpret_cast<std::uintptr_t>(&jit_memory::allocate);
	const auto start = reinterpret_cast<std::uintptr_t>(mapping);
	const auto block = image & ~(block_bytes - 1U);
	return start >= block && start + size - block <= block_bytes;
}

/* Writes `mov w0, #value; ret` and calls it. */
[[nodiscard]] std::uint32_t run(void *code, std::uint16_t value)
{
	const std::array<std::uint32_t, 2> words{
		0x52800000U | (std::uint32_t{value} << 5U), 0xd65f03c0U};
	jit_memory::set_writable(true);
	std::memcpy(code, words.data(), sizeof words);
	jit_memory::set_writable(false);
	jit_memory::invalidate_instruction_cache(code, sizeof words);
	return reinterpret_cast<std::uint32_t (*)()>(code)();
}

/* Data reservations made after the process asked for JIT memory fill the
 * image's block, as a JIT's own data does, and JIT memory stays near. */
void the_window_is_claimed_before_data_fills_the_block()
{
	constexpr std::size_t filler_bytes = buffer_bytes;
	std::vector<void *> fillers;
	for (;;) {
		void *filler = mmap(nullptr, filler_bytes, PROT_NONE,
			MAP_PRIVATE | MAP_ANON, -1, 0);
		require(filler != MAP_FAILED, "a data reservation failed");
		fillers.push_back(filler);
		if (!near_image(filler, filler_bytes))
			break;
		require(fillers.size() < 512, "the image's block never filled");
	}
	void *mapping = jit_memory::allocate(buffer_bytes);
	require(mapping != nullptr && near_image(mapping, buffer_bytes),
		"JIT memory left the image's block once data filled it");
	jit_memory::release(mapping, buffer_bytes);
	for (void *filler : fillers)
		(void)munmap(filler, filler_bytes);
}

void allocations_come_from_the_window_lowest_first()
{
	void *first = jit_memory::allocate(buffer_bytes);
	void *second = jit_memory::allocate(buffer_bytes);
	require(first != nullptr && second != nullptr, "allocation failed");
	require(near_image(first, buffer_bytes) &&
			near_image(second, buffer_bytes),
		"JIT memory lies outside this image's 4 GiB block");
	const auto a = reinterpret_cast<std::uintptr_t>(first);
	const auto b = reinterpret_cast<std::uintptr_t>(second);
	require(a + buffer_bytes <= b || b + buffer_bytes <= a,
		"two live allocations overlap");
	require(run(first, 41) == 41 && run(second, 42) == 42,
		"window memory did not run the code written to it");

	jit_memory::release(first, buffer_bytes);
	void *again = jit_memory::allocate(buffer_bytes);
	require(again == first, "a released range was not served again first");
	require(run(again, 43) == 43,
		"a range served again did not run the code written to it");
	jit_memory::release(again, buffer_bytes);
	jit_memory::release(second, buffer_bytes);
}

void a_full_window_falls_back_and_recovers()
{
	/* Far more than the window holds; each mapping is address space only. */
	constexpr std::size_t limit = 160;
	std::vector<void *> mappings;
	mappings.reserve(limit);
	bool spilled = false;
	while (!spilled && mappings.size() < limit) {
		void *mapping = jit_memory::allocate(buffer_bytes);
		require(mapping != nullptr, "allocation failed");
		mappings.push_back(mapping);
		spilled = !near_image(mapping, buffer_bytes);
	}
	require(spilled, "the window never filled");
	require(mappings.size() > 1, "the window held no 16 MiB buffer");
	require(run(mappings.back(), 44) == 44,
		"a mapping beyond the window did not run its code");
	for (void *mapping : mappings)
		jit_memory::release(mapping, buffer_bytes);

	void *mapping = jit_memory::allocate(buffer_bytes);
	require(mapping != nullptr && near_image(mapping, buffer_bytes),
		"the window did not serve again after its ranges were released");
	jit_memory::release(mapping, buffer_bytes);
}

} // namespace

int main()
{
	require(jit_memory::available(), "this host cannot run JIT code");
	the_window_is_claimed_before_data_fills_the_block();
	allocations_come_from_the_window_lowest_first();
	a_full_window_falls_back_and_recovers();
	std::puts("JIT memory window tests passed");
	return EXIT_SUCCESS;
}
