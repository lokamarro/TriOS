// SPDX-License-Identifier: GPL-2.0-or-later
/* The loader releases the kernel image when the kernel returns, and the
 * image's last pages are the first guest RAM, which every vhost-user backend's
 * borrowed view covers. A backend is stopped only after the run returns, so the
 * release keeps the image while a view borrows the RAM root and hands it back
 * with the last view; without one it releases at once. */
#include "kernel_signal_scope.hpp"
#include "mach_vm_region_test_support.hpp"
#include "memory/shared_backing.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using uml::testing::require;
using uml::testing::ScratchBacking;

/* VM_PROT_* of the page at address, or -1 when nothing maps it. */
[[nodiscard]] int protection_of(const void *address)
{
	mach_vm_address_t region = reinterpret_cast<mach_vm_address_t>(address);
	mach_vm_size_t size = 0;
	vm_region_basic_info_data_64_t information {};
	mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t object = MACH_PORT_NULL;
	const auto result = mach_vm_region(mach_task_self(), &region, &size,
		VM_REGION_BASIC_INFO_64,
		reinterpret_cast<vm_region_info_t>(&information), &count, &object);
	if (object != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), object);
	if (result != KERN_SUCCESS ||
	    region > reinterpret_cast<mach_vm_address_t>(address))
		return -1;
	return information.protection;
}

struct Layout final {
	std::uint8_t *root{};
	std::size_t page{};
	/* Image: pages 0..2; page 2 is the image tail the kernel gives to RAM.
	 * The suffix starts at page 4; page 3 is the materialized tail. */
	static constexpr std::size_t image_pages = 3;
	static constexpr std::size_t pages = 8;
};

/* The loader's image inside a reserved window, then the RAM root over it as
 * the kernel builds it: the suffix first, then the image tail in place. */
Layout build(const ScratchBacking &token, std::size_t page)
{
	require(::ftruncate(token.descriptor(),
		static_cast<off_t>(page * Layout::pages)) == 0);
	require(uml::memory::register_anonymous_shared_backing(token.descriptor()));
	void *window = ::mmap(nullptr, page * (Layout::pages + 2), PROT_NONE,
		MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	require(window != MAP_FAILED);
	Layout layout{static_cast<std::uint8_t *>(window), page};
	require(::mmap(layout.root, page * Layout::image_pages,
		PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) ==
		layout.root);
	auto *suffix = layout.root + page * 4;
	require(uml::memory::map_shared_backing(suffix, page * (Layout::pages - 4),
		PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, token.descriptor(),
		page * 4) == suffix);
	/* mem_init's in-place release of the image tail into guest RAM. */
	require(uml::memory::map_shared_backing(layout.root + page * 2, page * 2,
		PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, token.descriptor(),
		page * 2) == layout.root + page * 2);
	layout.root[page * 2] = 0x5a;
	return layout;
}

/* A backend's memory table starts at the image tail; while it lives, the
 * image release keeps the tail mapped, and the last view hands it back. */
void test_borrowed_tail_outlives_image_release(std::size_t page)
{
	const ScratchBacking token{"uml-kernel-image-release-XXXXXX", 0};
	const auto layout = build(token, page);
	{
		auto view = uml::memory::acquire_shared_backing_view(
			token.descriptor(), page * 2, page * (Layout::pages - 2),
			PROT_READ | PROT_WRITE);
		require(static_cast<bool>(view));
		uml::runtime::release_kernel_image(layout.root,
			page * Layout::image_pages, true);
		require(protection_of(layout.root + page * 2) ==
			(VM_PROT_READ | VM_PROT_WRITE));
		const auto *bytes = static_cast<volatile std::uint8_t *>(view.data());
		require(bytes[0] == 0x5a);
	}
	require(protection_of(layout.root) == VM_PROT_NONE);
	require(protection_of(layout.root + page * 2) == VM_PROT_NONE);
	/* The suffix is not the image's to release. */
	require(protection_of(layout.root + page * 4) ==
		(VM_PROT_READ | VM_PROT_WRITE));
}

/* Without a view the image goes at once: back to the reservation inside a
 * window, unmapped outside one. */
void test_unborrowed_image_is_released_at_once(std::size_t page)
{
	const ScratchBacking token{"uml-kernel-image-release-XXXXXX", 0};
	const auto layout = build(token, page);
	uml::runtime::release_kernel_image(layout.root, page * 2, false);
	require(protection_of(layout.root) == -1);
	uml::runtime::release_kernel_image(layout.root + page * 2, page, true);
	require(protection_of(layout.root + page * 2) == VM_PROT_NONE);
}

/* The first restart on the iPad: a backend's view outlived the run, so the
 * image release waited for the last view, and the image holds signed text
 * that iOS made permanent, which refuses any replacement that touches it.
 * Kept, the text stays mapped while the rest of the image goes back to the
 * reservation with the last view, and the loader's next placement of the
 * image reserves around it in the same way. */
void test_kept_text_survives_image_release(std::size_t page)
{
	const ScratchBacking token{"uml-kernel-image-release-XXXXXX", 0};
	const auto layout = build(token, page);
	auto *text = layout.root + page;
	require(uml::testing::allocate_permanent(text, page));
	text[0] = 0x7e;
	require(::mprotect(text, page, PROT_READ) == 0);
	require(!uml::runtime::signed_kernel_text_kept(text, page));
	require(uml::runtime::keep_signed_kernel_text(text, page));
	require(uml::runtime::signed_kernel_text_kept(text, page));
	{
		auto view = uml::memory::acquire_shared_backing_view(
			token.descriptor(), page * 2, page * (Layout::pages - 2),
			PROT_READ | PROT_WRITE);
		require(static_cast<bool>(view));
		uml::runtime::release_kernel_image(layout.root,
			page * Layout::image_pages, true);
		require(protection_of(layout.root) ==
			(VM_PROT_READ | VM_PROT_WRITE));
	}
	require(protection_of(layout.root) == VM_PROT_NONE);
	require(protection_of(text) == VM_PROT_READ && text[0] == 0x7e);
	require(protection_of(layout.root + page * 2) == VM_PROT_NONE);
	require(protection_of(layout.root + page * 4) ==
		(VM_PROT_READ | VM_PROT_WRITE));

	require(::mprotect(layout.root, page, PROT_READ | PROT_WRITE) == 0);
	require(uml::runtime::reserve_kernel_image(layout.root,
		page * Layout::image_pages) == 0);
	require(protection_of(layout.root) == VM_PROT_NONE);
	require(protection_of(text) == VM_PROT_READ && text[0] == 0x7e);
}

/* The hazard itself, as a control: releasing the image with a plain
 * reservation, as the loader once did, leaves a backend that reads the tail
 * through its view faulting (SIGBUS in a reservation). */
void test_previous_release_faults_a_borrowed_reader(std::size_t page)
{
	const ScratchBacking token{"uml-kernel-image-release-XXXXXX", 0};
	const auto layout = build(token, page);
	auto view = uml::memory::acquire_shared_backing_view(token.descriptor(),
		page * 2, page * (Layout::pages - 2), PROT_READ | PROT_WRITE);
	require(static_cast<bool>(view));
	require(uml::memory::reserve_host_range(layout.root,
		page * Layout::image_pages) == 0);
	/* The signal itself is the result, not a sanitizer's report of it. */
	require(::signal(SIGBUS, SIG_DFL) != SIG_ERR);
	const auto *bytes = static_cast<volatile std::uint8_t *>(view.data());
	std::printf("unreachable: read %u\n", bytes[0]);
}

void run_in_child(void (*test)(std::size_t), std::size_t page,
	int expected_signal)
{
	const pid_t child = ::fork();
	require(child >= 0);
	if (child == 0) {
		test(page);
		std::_Exit(EXIT_SUCCESS);
	}
	int status = 0;
	for (int attempt = 0;; ++attempt) {
		const pid_t waited = ::waitpid(child, &status, WNOHANG);
		require(waited >= 0);
		if (waited == child)
			break;
		if (attempt == 3000) {
			(void)::kill(child, SIGKILL);
			(void)::waitpid(child, &status, 0);
			require(false);
		}
		::usleep(10000);
	}
	if (expected_signal == 0)
		require(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
	else
		require(WIFSIGNALED(status) && WTERMSIG(status) == expected_signal);
}

} // namespace

int main()
{
	const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
	run_in_child(test_previous_release_faults_a_borrowed_reader, page, SIGBUS);
	run_in_child(test_borrowed_tail_outlives_image_release, page, 0);
	run_in_child(test_unborrowed_image_is_released_at_once, page, 0);
	run_in_child(test_kept_text_survives_image_release, page, 0);
	std::puts("kernel image release tests passed");
	return EXIT_SUCCESS;
}
