// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../shared_backing.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

struct stat;

namespace uml::memory {

class MappedBacking final {
public:
	static std::shared_ptr<MappedBacking> create(int fd,
		const struct stat &status);

	MappedBacking(const MappedBacking &) = delete;
	MappedBacking &operator=(const MappedBacking &) = delete;
	~MappedBacking();

	[[nodiscard]] std::byte *at(std::uint64_t offset) const noexcept;
	/* The address of offset 0: the inverse of at(), so the offset of an
	 * address inside the backing is the address less base(). */
	[[nodiscard]] std::byte *base() const noexcept
	{
		return static_cast<std::byte *>(mapping_.data());
	}
	[[nodiscard]] std::size_t size() const noexcept { return size_; }
	/* Code-page records are shared by every virtual alias of one physical
	 * backing page. Their addresses remain stable for the backing lifetime so
	 * the static-memory TLB can cache the opaque handle between mapping changes.
	 *
	 * Every translating vCPU asks for one before it reads a block's bytes and
	 * again whenever it revalidates a cached translation, so the answer is read
	 * far more often than a page is first seen. Because a record is never
	 * destroyed or moved, a lazily filled two-level index of atomic pointers
	 * answers the repeat without a lock; only a page seen for the first time
	 * takes one.
	 */
	[[nodiscard]] void *code_page_handle(std::uint64_t offset) noexcept;
	[[nodiscard]] static bool load_code_page_stamp(const void *handle,
		std::uint64_t &identity, std::uint64_t &version) noexcept;
	static void invalidate_code_page(void *handle) noexcept;
	/* Moves the stamp of the backing page at `offset` if a translator has
	 * ever asked for it. A page nothing stamped holds no translation whose
	 * stamp could be stale, so it is not given a record here. */
	void retire_code_page(std::uint64_t offset) noexcept;

private:
	struct CodePage;
	/* One index leaf covers this many backing pages. A leaf is 4 KiB of
	 * pointers, and the root is one pointer per leaf. */
	static constexpr std::size_t code_index_leaf_pages = 512;
	using CodeIndexLeaf =
		std::array<std::atomic<CodePage *>, code_index_leaf_pages>;

	MappedBacking(SharedBackingView mapping, std::size_t size) noexcept;
	[[nodiscard]] void *install_code_page(std::uint64_t page) noexcept;

	SharedBackingView mapping_;
	std::size_t size_;
	/* Taken only to publish a page seen for the first time. */
	std::mutex code_pages_mutex_;
	std::unordered_map<std::uint64_t, std::unique_ptr<CodePage>> code_pages_;
	std::vector<std::unique_ptr<CodeIndexLeaf>> code_index_leaves_;
	std::unique_ptr<std::atomic<CodeIndexLeaf *>[]> code_index_;
	std::size_t code_index_pages_{};
};

} // namespace uml::memory
