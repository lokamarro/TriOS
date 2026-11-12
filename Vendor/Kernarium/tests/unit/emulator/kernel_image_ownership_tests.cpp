// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Creation copies the kernel image, so a caller may free its buffer, with one
 * exception: the kernel a product links into itself as __TEXT,__uml_elf is
 * run in place, since a copy of it would be dirty memory for the handle's
 * life. This binary links such a section, as a product does, and the test
 * variant lets a test declare a further region to be one so that a kernel
 * the handle can run is borrowed too.
 */
#include "uml/emulator.h"
#include "emulator_test_support.hpp"
#include "require.hpp"
#include "testing.hpp"

#include <malloc/malloc.h>
#include <sys/mman.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/* The kernel image of a product, linked into this binary the way the
 * products link theirs: a read-only __TEXT section named by the linker's
 * section symbols. Assembly emits it, since a sanitizer pads a global. */
__asm__(".pushsection __TEXT,__uml_elf\n"
	"_uml_test_linked_kernel:\n"
	".fill 98304, 1, 0x5a\n"
	".no_dead_strip _uml_test_linked_kernel\n"
	".popsection\n");

extern "C" {
extern const unsigned char linked_kernel_start[]
	__asm("section$start$__TEXT$__uml_elf");
extern const unsigned char linked_kernel_end[]
	__asm("section$end$__TEXT$__uml_elf");
}

namespace {

constexpr std::size_t linked_kernel_size = std::size_t{96} << 10;

using uml::emulator::testing::kernel_image;
using uml::emulator::testing::kernel_image_borrowed;
using uml::emulator::testing::set_linked_kernel;
using uml::testing::create_emulator;
using uml::testing::require;

/* A sanitizer replaces the allocator, and the default zone then says nothing
 * about the program's own allocations. */
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool zone_is_measurable = false;
#else
constexpr bool zone_is_measurable = true;
#endif

constexpr int entered_status = 29;
constexpr std::size_t large_image = std::size_t{32} << 20;

int entered_kernel(int, char *const[], char *const[])
{
	return entered_status;
}

[[nodiscard]] std::size_t zone_in_use() noexcept
{
	malloc_statistics_t statistics{};
	malloc_zone_statistics(malloc_default_zone(), &statistics);
	return statistics.size_in_use;
}

[[nodiscard]] uml_emulator_config config_for(
	const void *image, std::size_t size) noexcept
{
	return {
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = "/tmp/uml-test.img",
		.kernel_image = image,
		.kernel_image_size = size,
		.network = nullptr,
	};
}

int run(uml_emulator *emulator)
{
	const char *arguments[]{"uml-kernel-image"};
	return uml_emulator_run(emulator, 1, arguments, nullptr);
}

/* Declares a region a linked kernel for as long as the test scope lasts. */
class LinkedKernelScope final {
public:
	LinkedKernelScope(const void *start, std::size_t size) noexcept
	{
		set_linked_kernel(start, size);
	}
	~LinkedKernelScope() { set_linked_kernel(nullptr, 0); }
	LinkedKernelScope(const LinkedKernelScope &) = delete;
	LinkedKernelScope &operator=(const LinkedKernelScope &) = delete;
};

/* Any other image is copied: the caller frees its buffer, even overwrites it,
 * and the handle still runs the kernel it was given. */
void test_a_caller_buffer_is_copied()
{
	auto image = kernel_image(entered_kernel);
	auto emulator = create_emulator(config_for(image.data(), image.size()));
	require(!kernel_image_borrowed(emulator.get()),
		"a caller's buffer was borrowed");
	std::memset(image.data(), 0xaa, image.size());
	image.clear();
	image.shrink_to_fit();
	require(run(emulator.get()) == entered_status,
		"the handle did not keep its own copy of the image");
	require(run(emulator.get()) == entered_status,
		"a second run did not find the copy");
}

/* A linked kernel is run in place: no copy, and runs enter it. The handle
 * never frees it, and it stays whole after the handle ends. */
void test_a_linked_kernel_is_borrowed()
{
	const auto linked = kernel_image(entered_kernel);
	const LinkedKernelScope scope{linked.data(), linked.size()};
	{
		auto emulator = create_emulator(
			config_for(linked.data(), linked.size()));
		require(kernel_image_borrowed(emulator.get()),
			"a linked kernel was copied");
		require(run(emulator.get()) == entered_status,
			"a handle did not run the borrowed kernel");
		require(run(emulator.get()) == entered_status,
			"a second run did not find the borrowed kernel");
	}
	require(linked == kernel_image(entered_kernel),
		"a handle changed or freed the linked kernel");
}

/* Only the linked kernel itself is borrowed: not a prefix of it, and not
 * another buffer that holds the same bytes. */
void test_only_the_linked_kernel_itself_is_borrowed()
{
	const auto linked = kernel_image(entered_kernel);
	const LinkedKernelScope scope{linked.data(), linked.size()};
	{
		auto emulator = create_emulator(
			config_for(linked.data(), linked.size() - 1));
		require(!kernel_image_borrowed(emulator.get()),
			"a prefix of the linked kernel was borrowed");
	}
	{
		auto twin = kernel_image(entered_kernel);
		auto emulator = create_emulator(config_for(twin.data(), twin.size()));
		require(!kernel_image_borrowed(emulator.get()),
			"a buffer with the linked kernel's bytes was borrowed");
		std::memset(twin.data(), 0xaa, twin.size());
		twin.clear();
		twin.shrink_to_fit();
		require(run(emulator.get()) == entered_status,
			"a twin of the linked kernel was not copied");
	}
}

/* The section this binary links is found with no help from the test variant:
 * the same lookup that finds a product's kernel. */
void test_the_section_this_binary_links_is_borrowed()
{
	const auto size = static_cast<std::size_t>(
		linked_kernel_end - linked_kernel_start);
	require(size == linked_kernel_size && linked_kernel_start[0] == 0x5a &&
			linked_kernel_start[size - 1] == 0x5a,
		"the test kernel is not the section");
	{
		auto emulator = create_emulator(
			config_for(linked_kernel_start, size));
		require(kernel_image_borrowed(emulator.get()),
			"the section this binary links was copied");
	}
	{
		auto emulator = create_emulator(
			config_for(linked_kernel_start, size - 1));
		require(!kernel_image_borrowed(emulator.get()),
			"a prefix of the section was borrowed");
	}
	{
		auto emulator = create_emulator(
			config_for(linked_kernel_start + 1, size - 1));
		require(!kernel_image_borrowed(emulator.get()),
			"a suffix of the section was borrowed");
	}
	{
		std::vector<unsigned char> twin(linked_kernel_start,
			linked_kernel_start + size);
		auto emulator = create_emulator(config_for(twin.data(), size));
		require(!kernel_image_borrowed(emulator.get()),
			"a buffer with the section's bytes was borrowed");
	}
}

/* A kernel-sized image: a copy is dirty memory the size of the image for the
 * handle's life, and a linked kernel costs the handle nothing. */
void test_a_copy_costs_the_image_and_a_linked_kernel_costs_nothing()
{
	void *const region = mmap(nullptr, large_image, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	require(region != MAP_FAILED, "could not map a kernel-sized image");
	std::memset(region, 0x5a, large_image);
	require(mprotect(region, large_image, PROT_READ) == 0,
		"could not make the image immutable");

	const auto before = zone_in_use();
	{
		auto emulator = create_emulator(config_for(region, large_image));
		require(!kernel_image_borrowed(emulator.get()),
			"an undeclared image was borrowed");
		const auto copied = zone_in_use() - before;
		std::printf("copied: the handle holds %zu bytes\n", copied);
		require(!zone_is_measurable || copied >= large_image,
			"a copy did not cost the image");
	}
	require(!zone_is_measurable || zone_in_use() < before + (1U << 20),
		"the copy outlived its handle");

	{
		const LinkedKernelScope scope{region, large_image};
		auto emulator = create_emulator(config_for(region, large_image));
		require(kernel_image_borrowed(emulator.get()),
			"the declared linked kernel was copied");
		const auto borrowed = zone_in_use() - before;
		std::printf("borrowed: the handle holds %zu bytes\n", borrowed);
		require(!zone_is_measurable || borrowed < (1U << 20),
			"borrowing the linked kernel cost memory");
	}
	require(munmap(region, large_image) == 0);
}

} // namespace

int main()
{
	test_a_caller_buffer_is_copied();
	test_a_linked_kernel_is_borrowed();
	test_only_the_linked_kernel_itself_is_borrowed();
	test_the_section_this_binary_links_is_borrowed();
	test_a_copy_costs_the_image_and_a_linked_kernel_costs_nothing();
	std::puts("kernel image ownership tests passed");
	return EXIT_SUCCESS;
}
