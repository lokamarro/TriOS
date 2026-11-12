// SPDX-License-Identifier: GPL-2.0-or-later
/* A run ends by retiring its native address space, and by then the loader has
 * already released the kernel image. The first pages of guest RAM are that
 * image's tail, so the RAM root's span is no longer all mapped, and a shadow
 * of those pages must not be published into it. The loader leaves the window
 * kept, re-reserved PROT_NONE (iOS) or unmapped (macOS); the root is also made
 * unreadable in place, which keeps its contents so that a publication into it
 * can be told from none.
 *
 * Retirement discards a shadow of guest RAM, which has no reader once the run
 * is over, and still publishes a shadow of any other backing, which outlives
 * it; native_mmu_tests covers that half.
 *
 * The space keeps its indexes in a heap it maps itself, so that retirement
 * gives their memory back to the process whole instead of leaving it to the
 * default malloc zone, which returns it when it chooses. */
#include "native_address_space.hpp"
#include "domain_contract.hpp"
#include "domain_heap.hpp"
#include "native_window_test_support.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"
#include "memory/shared_backing.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <malloc/malloc.h>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr std::size_t guest_page_size = 4096;
constexpr std::size_t marker_offset = 137;
constexpr std::byte original{0x31};
constexpr std::byte dirty{0x42};

using uml::testing::ScratchBacking;
using uml::testing::require;

enum class Window {
	kept,
	/* The root stays mapped, with its contents, but cannot be read. */
	unreadable,
	/* The loader's iOS release: the range goes back to the reservation. */
	reserved,
	/* The loader's macOS release: the range is unmapped. */
	unmapped,
};

enum class Path {
	/* retire() itself. */
	retire,
	/* The last reference goes away, as for a space on a stack. */
	destructor,
	/* The run's end: Context::end_run() retires the memory domain. */
	domain,
};

constexpr const char *name(Window window) noexcept
{
	switch (window) {
	case Window::kept:
		return "kept";
	case Window::unreadable:
		return "unreadable";
	case Window::reserved:
		return "reserved";
	case Window::unmapped:
		return "unmapped";
	}
	return "";
}

constexpr const char *name(Path path) noexcept
{
	switch (path) {
	case Path::retire:
		return "retire";
	case Path::destructor:
		return "destructor";
	case Path::domain:
		return "domain";
	}
	return "";
}

std::atomic_uint64_t active_domain{};
std::array<char, 96> case_name{};

/* A sanitizer replaces the allocator, and the default zone then says nothing
 * about the program's own allocations. */
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool zone_is_measurable = false;
#else
constexpr bool zone_is_measurable = true;
#endif

[[nodiscard]] std::size_t zone_in_use() noexcept
{
	malloc_statistics_t statistics{};
	malloc_zone_statistics(malloc_default_zone(), &statistics);
	return statistics.size_in_use;
}

bool provide_test_domain(std::uint64_t &domain) noexcept
{
	domain = active_domain.load(std::memory_order_acquire);
	return domain != 0;
}

/* A read of released memory is the failure under test; name the case instead
 * of leaving a crash report. */
void report_fault(int signal, siginfo_t *, void *) noexcept
{
	std::array<char, 192> message{};
	const int length = std::snprintf(message.data(), message.size(),
		"native-retire: signal %d in case %s: retirement touched guest RAM "
		"the loader had released\n", signal, case_name.data());
	if (length > 0)
		(void)!write(STDERR_FILENO, message.data(),
			static_cast<std::size_t>(length));
	_exit(1);
}

void install_fault_reporter()
{
	struct sigaction action {};
	action.sa_sigaction = &report_fault;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	require(sigaction(SIGSEGV, &action, nullptr) == 0 &&
		sigaction(SIGBUS, &action, nullptr) == 0,
		"fault reporter installation failed");
}

/* Address space in the native window that nothing maps, as the kernel's
 * physical window and its shadow slots are before the guest takes them. */
class WindowRange final {
public:
	explicit WindowRange(std::size_t size) : size_{size}
	{
		address_ = uml::testing::reserve_native_test_window(size);
		require(address_ != 0, "native test window reservation failed");
		require(uml::testing::release_native_test_window(address_, size),
			"native test window release failed");
	}

	~WindowRange()
	{
		(void)munmap(reinterpret_cast<void *>(address_), size_);
	}

	WindowRange(const WindowRange &) = delete;
	WindowRange &operator=(const WindowRange &) = delete;

	[[nodiscard]] std::uintptr_t address() const noexcept { return address_; }
	[[nodiscard]] std::size_t size() const noexcept { return size_; }
	[[nodiscard]] void *pointer() const noexcept
	{
		return reinterpret_cast<void *>(address_);
	}

private:
	std::uintptr_t address_{};
	std::size_t size_{};
};

class GuestRam final {
public:
	GuestRam()
		: token_{"uml-native-retire.XXXXXX", 4 * guest_page_size},
		  root_{4 * guest_page_size}
	{
		require(uml::memory::register_anonymous_shared_backing(
				token_.descriptor()),
			"anonymous backing registration failed");
		require(uml::memory::map_shared_backing(root_.pointer(),
				root_.size(), PROT_READ | PROT_WRITE,
				MAP_FIXED | MAP_SHARED, token_.descriptor(), 0) ==
				root_.pointer(),
			"anonymous backing root establishment failed");
	}

	void run(Window window, Path path)
	{
		static std::uint64_t next_domain = 40'000;
		(void)std::snprintf(case_name.data(), case_name.size(),
			"%s over a %s window", name(path), name(window));

		set_root_byte(original);
		const WindowRange slot{4 * guest_page_size};
		std::uint64_t domain = 0;
		std::shared_ptr<uml::memory::NativeAddressSpace> space;
		if (path == Path::domain) {
			domain = next_domain++;
			active_domain.store(domain, std::memory_order_release);
			space = uml::memory::native_address_space();
		} else {
			space = std::make_shared<uml::memory::NativeAddressSpace>();
		}
		require(space != nullptr, "native address space was not created");
		require(space->map(slot.address(), token_.descriptor(), 0,
				guest_page_size, PROT_READ | PROT_WRITE) == 0,
			"shadow of guest RAM was not mapped");
		auto *shadow = reinterpret_cast<volatile std::byte *>(slot.address());
		require(shadow[marker_offset] == original,
			"shadow did not load guest RAM");
		shadow[marker_offset] = dirty;
		require(space->materialized_slot_count_for_testing() == 1,
			"the shadow did not materialize its slot");

		release(window);
		switch (path) {
		case Path::retire:
			space->retire();
			break;
		case Path::destructor:
			space.reset();
			break;
		case Path::domain:
			active_domain.store(0, std::memory_order_release);
			uml::memory::retire_domain(domain);
			break;
		}

		if (space != nullptr) {
			require(space->mapping_count_for_testing() == 0 &&
				space->materialized_slot_count_for_testing() == 0,
				"a retired space kept shadow state");
			errno = 0;
			require(space->publish_backing(token_.descriptor(), 0,
					guest_page_size) == -1 && errno == ESHUTDOWN,
				"a retired space accepted a publication");
		}

		/* A root the loader released comes back empty. */
		const bool contents_kept =
			window == Window::kept || window == Window::unreadable;
		const auto expected = contents_kept ? original : std::byte{0};
		restore(window);
		require(root_byte() == expected,
			"retirement published a shadow into guest RAM");

		/* The slot is free again, and its next owner sees guest RAM, not the
		 * retired space's stores. */
		uml::memory::NativeAddressSpace successor;
		require(successor.map(slot.address(), token_.descriptor(), 0,
				guest_page_size, PROT_READ | PROT_WRITE) == 0,
			"a successor could not claim the retired space's slot");
		require(shadow[marker_offset] == expected,
			"a successor read the retired space's unpublished store");
		require(successor.unmap(slot.address(), guest_page_size) == 0,
			"successor cleanup failed");
		require(root_byte() == expected,
			"a successor's clean shadow changed guest RAM");
	}

	/* Many shadows, the way a booted guest has them: the indexes grow with the
	 * pages and they all live in the space's heap. The default zone does not
	 * grow with them, and when the space ends, by any path, the heap is gone
	 * and the zone is where it was. */
	void run_heap(Path path)
	{
		static std::uint64_t next_domain = 50'000;
		(void)std::snprintf(case_name.data(), case_name.size(),
			"%s with many shadows", name(path));
		constexpr std::size_t pages = 1024;
		constexpr std::size_t host_page = 16384;
		const WindowRange range{pages * host_page};
		auto &region = uml::memory::HeapRegion::instance();
		const auto heap_before = region.committed();
		const auto zone_before = zone_in_use();

		std::uint64_t domain = 0;
		std::shared_ptr<uml::memory::NativeAddressSpace> space;
		if (path == Path::domain) {
			domain = next_domain++;
			active_domain.store(domain, std::memory_order_release);
			space = uml::memory::native_address_space();
		} else {
			space = std::make_shared<uml::memory::NativeAddressSpace>();
		}
		require(space != nullptr, "native address space was not created");
		/* The space itself, its filter included, is the zone's. */
		const auto zone_with_space = zone_in_use();
		for (std::size_t page = 0; page < pages; ++page)
			require(space->map(range.address() + page * host_page,
					token_.descriptor(), 0, guest_page_size,
					PROT_READ | PROT_WRITE) == 0,
				"a shadow was not mapped");
		require(space->mapping_count_for_testing() == pages);
		/* Each mapping keeps a 4 KiB copy of its page. */
		require(region.committed() - heap_before >= pages * guest_page_size,
			"the space's indexes are not in its heap");
		require(!zone_is_measurable ||
				zone_in_use() < zone_with_space + (256U << 10),
			"the space's indexes grew the default zone");

		switch (path) {
		case Path::retire:
			space->retire();
			break;
		case Path::destructor:
			space.reset();
			break;
		case Path::domain:
			active_domain.store(0, std::memory_order_release);
			uml::memory::retire_domain(domain);
			break;
		}
		require(region.committed() == heap_before,
			"a retired space left heap pages committed");
		if (space != nullptr) {
			require(space->mapping_count_for_testing() == 0);
			space.reset();
		}
		require(region.committed() == heap_before);
		require(!zone_is_measurable ||
				zone_in_use() < zone_before + (64U << 10),
			"the default zone did not return to where it was");
	}

private:
	void set_root_byte(std::byte value) const noexcept
	{
		static_cast<volatile std::byte *>(root_.pointer())[marker_offset] =
			value;
	}

	[[nodiscard]] std::byte root_byte() const noexcept
	{
		return static_cast<volatile std::byte *>(
			root_.pointer())[marker_offset];
	}

	void release(Window window) const
	{
		switch (window) {
		case Window::kept:
			break;
		case Window::unreadable:
			require(mprotect(root_.pointer(), root_.size(), PROT_NONE) == 0,
				"guest RAM could not be made unreadable");
			break;
		case Window::reserved:
			require(mmap(root_.pointer(), root_.size(), PROT_NONE,
					MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE,
					-1, 0) == root_.pointer(),
				"guest RAM could not be re-reserved");
			break;
		case Window::unmapped:
			require(munmap(root_.pointer(), root_.size()) == 0,
				"guest RAM could not be unmapped");
			break;
		}
	}

	void restore(Window window) const
	{
		switch (window) {
		case Window::kept:
			break;
		case Window::unreadable:
			require(mprotect(root_.pointer(), root_.size(),
					PROT_READ | PROT_WRITE) == 0,
				"guest RAM could not be made readable again");
			break;
		case Window::reserved:
		case Window::unmapped:
			require(mmap(root_.pointer(), root_.size(),
					PROT_READ | PROT_WRITE,
					MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) ==
					root_.pointer(),
				"guest RAM could not be mapped again");
			break;
		}
	}

	ScratchBacking token_;
	WindowRange root_;
};

} // namespace

int main()
{
	install_fault_reporter();
	uml::testing::NativeTestWindowScope native_window;
	require(native_window.entered(),
		"could not configure native MMU test window");
	uml::memory::set_domain_provider(&provide_test_domain);

	GuestRam guest;
	for (const auto path : {Path::retire, Path::destructor, Path::domain})
		for (const auto window : {Window::kept, Window::unreadable,
			     Window::reserved, Window::unmapped})
			guest.run(window, path);
	for (const auto path : {Path::retire, Path::destructor, Path::domain})
		guest.run_heap(path);
	return 0;
}
