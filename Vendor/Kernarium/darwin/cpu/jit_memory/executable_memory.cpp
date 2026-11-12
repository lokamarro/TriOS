// SPDX-License-Identifier: GPL-2.0-or-later
#include "executable_memory.hpp"

#include <sys/mman.h>

#include <libkern/OSCacheControl.h>
#include <os/lock.h>
#include <pthread.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::jit_memory {
namespace {

/* Publication route for MAP_JIT memory.
 *
 * macOS declares pthread_jit_write_protect_np and calls it directly. No iOS
 * product has a publication route: a physical device cannot obtain the
 * entitlement, so JIT memory reports itself unavailable there rather than
 * mapping memory it could never publish into.
 *
 * The Simulator is deliberately held to the same answer even though it runs on
 * macOS and could reach the toggle dynamically. Letting it translate through a
 * path production devices cannot use would make the Simulator unrepresentative
 * of the product it exists to stand in for.
 */
#if defined(__APPLE__) && defined(__aarch64__) && defined(MAP_JIT) && \
	!defined(__IPHONE_OS_VERSION_MAX_ALLOWED)
void jit_write_protect(int enable)
{
	pthread_jit_write_protect_np(enable);
}

constexpr bool jit_publication_supported() noexcept
{
	return true;
}
#else
constexpr void jit_write_protect(int) {}

constexpr bool jit_publication_supported() noexcept
{
	return false;
}
#endif

/* Where JIT code lives.
 *
 * An Apple core predicts an indirect branch's target cheaply only when the
 * target lies near the branch. On an M1 Ultra a call through a register and
 * its return cost about 0.7 ns when caller and callee share an aligned
 * 512 MiB block, 1.9 ns when they share an aligned 4 GiB block, and 5 ns or
 * more when they do not. Generated code calls into this image -- FEXCore
 * reaches its x87 softfloat fallbacks that way for every x87 operation -- and
 * jumps between its own allocations, so where a mapping lands decides how fast
 * the code in it runs.
 *
 * The kernel places a mapping at the lowest free range. The 2 GiB between this
 * image and the shared cache fill with the JIT's own data, so a code buffer
 * mapped later in a run landed some 400 GiB away, and the guest process it
 * served ran x87 code a third slower than its neighbours.
 *
 * So the process claims one window, the largest, up to 1.5 GiB in 64 MiB
 * steps, whose lowest free range at or above this image's 512 MiB block lies
 * in its 4 GiB block, and serves every allocation from it, lowest address
 * first. It claims the window when it first asks whether it can run JIT
 * code, which it does when it selects a JIT engine, before its kernel boots:
 * by the first allocation the range is already scattered with small
 * mappings, and the JIT's data reservations claim the rest of it as soon as
 * the first contexts exist. FEXCore keeps a 16 MiB code buffer for each of
 * the eight address spaces each vCPU caches, so ten vCPUs keep 80 of them.
 *
 * MAP_JIT cannot map over an existing range, and a hole in a live window
 * would be the kernel's first choice for an unrelated mapping, so a release
 * keeps the pages mapped and marks them reusable; memory served again may
 * still hold what it held before. When the window is full, or cannot be
 * placed, a mapping goes wherever the kernel puts it.
 */
class Window final {
public:
	void claim() noexcept
	{
		os_unfair_lock_lock(&lock_);
		claim_locked();
		os_unfair_lock_unlock(&lock_);
	}

	[[nodiscard]] void *allocate(std::size_t size) noexcept
	{
		if (size == 0 || size > max_bytes)
			return nullptr;
		const auto count = granules_for(size);
		os_unfair_lock_lock(&lock_);
		claim_locked();
		const auto first = find(count);
		if (first != none)
			mark(first, count, true);
		os_unfair_lock_unlock(&lock_);
		if (first == none)
			return nullptr;
		void *mapping = base_ + first * granule;
		/* A released range is reusable: the kernel may discard its pages
		 * until it is claimed back. */
		(void)madvise(mapping, count * granule, MADV_FREE_REUSE);
		return mapping;
	}

	/* Whether mapping came from the window; if so its pages return to it. */
	[[nodiscard]] bool release(void *mapping, std::size_t size) noexcept
	{
		os_unfair_lock_lock(&lock_);
		std::byte *const base = base_;
		const std::size_t granules = granules_;
		os_unfair_lock_unlock(&lock_);
		auto *const bytes = static_cast<std::byte *>(mapping);
		if (base == nullptr || bytes < base ||
		    bytes >= base + granules * granule)
			return false;
		const auto first =
			static_cast<std::size_t>(bytes - base) / granule;
		const auto count = granules_for(size);
		if (count > granules - first)
			return true;
		/* Marked reusable before it is free, so no later claim can
		 * race the advice. */
		(void)madvise(mapping, count * granule, MADV_FREE_REUSABLE);
		os_unfair_lock_lock(&lock_);
		mark(first, count, false);
		os_unfair_lock_unlock(&lock_);
		return true;
	}

private:
	/* The host page on every Apple arm64 product. */
	static constexpr std::size_t granule = 16U * 1024U;
	static constexpr std::size_t step_bytes = 64U * 1024U * 1024U;
	static constexpr std::size_t max_bytes = 24U * step_bytes;
	static constexpr std::uintptr_t prediction_block = std::uintptr_t{1} << 29U;
	static constexpr std::uintptr_t near_block = std::uintptr_t{1} << 32U;
	static constexpr std::size_t none = ~std::size_t{0};

	[[nodiscard]] static constexpr std::size_t granules_for(
		std::size_t size) noexcept
	{
		return (size + granule - 1U) / granule;
	}

	void claim_locked() noexcept
	{
		if (claimed_)
			return;
		claimed_ = true;
		if (static_cast<std::size_t>(getpagesize()) > granule)
			return;
		/* Any function of this image locates it. */
		const auto image = reinterpret_cast<std::uintptr_t>(
			&::uml::cpu::jit_memory::allocate);
		const auto hint = image & ~(prediction_block - 1U);
		const auto block = image & ~(near_block - 1U);
		for (auto bytes = max_bytes; bytes != 0; bytes -= step_bytes) {
			void *mapping = mmap(reinterpret_cast<void *>(hint), bytes,
				PROT_READ | PROT_WRITE | PROT_EXEC,
				MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
			if (mapping == MAP_FAILED)
				continue;
			const auto start = reinterpret_cast<std::uintptr_t>(mapping);
			if (start >= block && start - block <= near_block - bytes) {
				base_ = static_cast<std::byte *>(mapping);
				granules_ = bytes / granule;
				return;
			}
			(void)munmap(mapping, bytes);
		}
	}

	[[nodiscard]] std::size_t find(std::size_t count) const noexcept
	{
		std::size_t run = 0;
		for (std::size_t i = 0; i < granules_;) {
			const auto word = used_[i / 64U];
			if (i % 64U == 0 && word == ~std::uint64_t{0}) {
				run = 0;
				i += 64U;
				continue;
			}
			if (((word >> (i % 64U)) & 1U) != 0)
				run = 0;
			else if (++run == count)
				return i + 1U - count;
			++i;
		}
		return none;
	}

	void mark(std::size_t first, std::size_t count, bool used) noexcept
	{
		for (auto i = first; i < first + count; ++i) {
			const auto bit = std::uint64_t{1} << (i % 64U);
			if (used)
				used_[i / 64U] |= bit;
			else
				used_[i / 64U] &= ~bit;
		}
	}

	os_unfair_lock lock_ = OS_UNFAIR_LOCK_INIT;
	bool claimed_{};
	std::byte *base_{};
	std::size_t granules_{};
	std::array<std::uint64_t, max_bytes / granule / 64U> used_{};
};

constinit Window window;

} // namespace

bool available() noexcept
{
	static const bool supported = [] {
#if defined(__APPLE__) && defined(__aarch64__) && defined(MAP_JIT)
		/* A successful MAP_JIT mapping is necessary but not sufficient:
		 * without a W^X toggle the first store into it faults. Requiring
		 * the publication route keeps this from promising an engine that
		 * would abort on its first translated block.
		 */
		if (!jit_publication_supported())
			return false;
		void *mapping = mmap(nullptr, 16U * 1024U,
			PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
		if (mapping == MAP_FAILED)
			return false;
		(void)munmap(mapping, 16U * 1024U);
		window.claim();
		return true;
#else
		return false;
#endif
	}();
	return supported;
}

void *allocate(std::size_t size) noexcept
{
	if constexpr (jit_publication_supported()) {
		if (void *mapping = window.allocate(size); mapping != nullptr)
			return mapping;
	}
	void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
	return mapping == MAP_FAILED ? nullptr : mapping;
}

void release(void *mapping, std::size_t size)
{
	if constexpr (jit_publication_supported()) {
		if (window.release(mapping, size))
			return;
	}
	(void)munmap(mapping, size);
}

void set_writable(bool writable)
{
	jit_write_protect(writable ? 0 : 1);
}

void invalidate_instruction_cache(void *code, std::size_t size)
{
	sys_icache_invalidate(code, size);
}

void *reserve_guarded(std::size_t size) noexcept
{
	const auto guard = static_cast<std::size_t>(getpagesize());
	if (size == 0 || size % guard != 0 || size > SIZE_MAX - 2 * guard)
		return nullptr;
	void *mapping = mmap(nullptr, size + 2 * guard, PROT_NONE,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	if (mapping == MAP_FAILED)
		return nullptr;
	auto *data = static_cast<std::byte *>(mapping) + guard;
	if (mprotect(data, size, PROT_READ | PROT_WRITE) != 0) {
		(void)munmap(mapping, size + 2 * guard);
		return nullptr;
	}
	return data;
}

void release_guarded(void *data, std::size_t size)
{
	const auto guard = static_cast<std::size_t>(getpagesize());
	(void)munmap(static_cast<std::byte *>(data) - guard, size + 2 * guard);
}

} // namespace uml::cpu::jit_memory
