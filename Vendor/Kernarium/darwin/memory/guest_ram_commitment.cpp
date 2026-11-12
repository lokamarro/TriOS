// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_ram_commitment.hpp"

#include "shared_backing.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <sys/mman.h>

namespace uml::memory {
namespace {

constexpr std::size_t bits_per_word = 64;

std::atomic<GuestRamCommitment *> published_commitment{nullptr};

[[nodiscard]] std::uint64_t word_mask(std::size_t first, std::size_t count)
{
	return count == bits_per_word ? ~std::uint64_t{0} :
		((std::uint64_t{1} << count) - 1) << first;
}

} // namespace

GuestRamCommitment::GuestRamCommitment(std::byte *physical_root,
	std::size_t size, std::size_t managed_begin,
	std::size_t uncommitted_begin, bool wire, std::size_t page_size)
	: root_(physical_root), size_(size), managed_begin_(managed_begin),
	  page_size_(page_size), page_count_(size / page_size), wire_(wire),
	  released_(std::make_unique<std::atomic<std::uint64_t>[]>(
		  (size / page_size + bits_per_word - 1) / bits_per_word))
{
	const std::size_t words =
		(page_count_ + bits_per_word - 1) / bits_per_word;
	for (std::size_t word = 0; word < words; ++word)
		released_[word].store(0, std::memory_order_relaxed);
	const std::size_t first =
		std::min(std::max(uncommitted_begin, managed_begin), size) /
		page_size;
	for (std::size_t page = first; page < page_count_; ++page)
		released_[page / bits_per_word].fetch_or(
			std::uint64_t{1} << (page % bits_per_word),
			std::memory_order_relaxed);
	released_pages_.store(page_count_ - first, std::memory_order_relaxed);
}

int GuestRamCommitment::release(std::uint64_t offset,
	std::uint64_t length) noexcept
{
	return change(Change::release, offset, length);
}

int GuestRamCommitment::commit(std::uint64_t offset,
	std::uint64_t length) noexcept
{
	return change(Change::commit, offset, length);
}

std::size_t GuestRamCommitment::managed_begin() const noexcept
{
	return std::min(managed_begin_, size_);
}

std::size_t GuestRamCommitment::managed_bytes() const noexcept
{
	return size_ - std::min(managed_begin_, size_);
}

std::size_t GuestRamCommitment::released_bytes() const noexcept
{
	return released_pages_.load(std::memory_order_relaxed) * page_size_;
}

std::uint64_t GuestRamCommitment::wire_failures() const noexcept
{
	return wire_failures_.load(std::memory_order_relaxed);
}

std::size_t GuestRamCommitment::word_count() const noexcept
{
	return (page_count_ + bits_per_word - 1) / bits_per_word;
}

std::uint64_t GuestRamCommitment::held_word(std::size_t word) const noexcept
{
	if (word >= word_count())
		return 0;
	const std::size_t first_managed =
		(managed_begin() + page_size_ - 1) / page_size_;
	const std::size_t low = word * bits_per_word;
	const std::size_t first = std::max(low, first_managed);
	const std::size_t last = std::min(low + bits_per_word, page_count_);
	if (first >= last)
		return 0;
	return word_mask(first - low, last - first) &
		~released_[word].load(std::memory_order_acquire);
}

void GuestRamCommitment::hold_changes() noexcept
{
	changes_held_.store(true, std::memory_order_seq_cst);
}

void GuestRamCommitment::allow_changes() noexcept
{
	changes_held_.store(false, std::memory_order_release);
	changes_held_.notify_all();
}

std::size_t GuestRamCommitment::changes_in_flight() const noexcept
{
	return in_flight_.load(std::memory_order_seq_cst);
}

int GuestRamCommitment::evict(std::size_t first_page,
	std::size_t pages) noexcept
{
	std::byte *const address = root_ + first_page * page_size_;
	const std::size_t length = pages * page_size_;
	/* As a release does: a wired page cannot leave the footprint, and a page
	 * marked reusable while wired would stay wired and uncounted. */
	if (wire_)
		(void)munlock(address, length);
	if (madvise(address, length, MADV_FREE_REUSABLE) != 0) {
		const int error = errno;
		report_mapping_failure("guest RAM evict", address, length, error);
		errno = error;
		return -1;
	}
	return 0;
}

int GuestRamCommitment::readmit(std::size_t first_page,
	std::size_t pages) noexcept
{
	std::byte *const address = root_ + first_page * page_size_;
	const std::size_t length = pages * page_size_;
	if (madvise(address, length, MADV_FREE_REUSE) != 0) {
		const int error = errno;
		report_mapping_failure("guest RAM readmit", address, length, error);
		errno = error;
		return -1;
	}
	/* As a commit does: the page stays usable when it cannot be wired, only
	 * compressible. */
	if (wire_ && wire_host_range(address, length) != 0) {
		const int error = errno;
		wire_failures_.fetch_add(1, std::memory_order_relaxed);
		report_mapping_failure("guest RAM wire", address, length, error);
	}
	return 0;
}

int GuestRamCommitment::change(Change kind, std::uint64_t offset,
	std::uint64_t length) noexcept
{
	if (offset % page_size_ != 0 || length % page_size_ != 0 ||
	    offset > size_ || length > size_ - offset) {
		errno = EINVAL;
		return -1;
	}
	std::uint64_t begin = std::max<std::uint64_t>(offset, managed_begin_);
	const std::uint64_t end = offset + length;
	if (begin % page_size_ != 0)
		begin += page_size_ - begin % page_size_;
	if (begin >= end)
		return 0;
	/* A snapshot of the held pages is out: nothing may change them until it
	 * has been put back. The count and the flag are sequentially consistent
	 * so that a holder either sees this call in flight or this call sees the
	 * hold. */
	in_flight_.fetch_add(1, std::memory_order_seq_cst);
	while (changes_held_.load(std::memory_order_seq_cst)) {
		in_flight_.fetch_sub(1, std::memory_order_seq_cst);
		changes_held_.wait(true, std::memory_order_acquire);
		in_flight_.fetch_add(1, std::memory_order_seq_cst);
	}
	struct Leave final {
		std::atomic<std::size_t> &count;
		~Leave() { count.fetch_sub(1, std::memory_order_seq_cst); }
	} leave{in_flight_};

	/* Flip each page's bit atomically and apply only the pages this call
	 * changed, merging adjacent ones into one host call. */
	std::size_t run_first = 0;
	std::size_t run_pages = 0;
	int result = 0;
	const auto flush = [&]() noexcept {
		if (run_pages != 0 && apply(kind, run_first, run_pages) != 0)
			result = -1;
		run_pages = 0;
	};
	std::size_t page = begin / page_size_;
	const std::size_t last = end / page_size_;
	while (page < last) {
		const std::size_t word = page / bits_per_word;
		const std::size_t bit = page % bits_per_word;
		const std::size_t count =
			std::min(bits_per_word - bit, last - page);
		const std::uint64_t mask = word_mask(bit, count);
		std::uint64_t changed;
		if (kind == Change::release)
			changed = mask & ~released_[word].fetch_or(
				mask, std::memory_order_acq_rel);
		else
			changed = mask & released_[word].fetch_and(
				~mask, std::memory_order_acq_rel);
		const auto flipped = static_cast<std::size_t>(
			std::popcount(changed));
		if (kind == Change::release)
			released_pages_.fetch_add(flipped, std::memory_order_relaxed);
		else
			released_pages_.fetch_sub(flipped, std::memory_order_relaxed);
		while (changed != 0) {
			const auto start = static_cast<std::size_t>(
				std::countr_zero(changed));
			const auto ones = static_cast<std::size_t>(
				std::countr_one(changed >> start));
			const std::size_t first = word * bits_per_word + start;
			if (run_pages != 0 && run_first + run_pages != first)
				flush();
			if (run_pages == 0)
				run_first = first;
			run_pages += ones;
			changed = start + ones == bits_per_word ? 0 :
				changed & ~word_mask(start, ones);
		}
		page += count;
	}
	flush();
	return result;
}

int GuestRamCommitment::apply(Change kind, std::size_t first_page,
	std::size_t pages) noexcept
{
	std::byte *const address = root_ + first_page * page_size_;
	const std::size_t length = pages * page_size_;
	if (kind == Change::release) {
		/* A wired page cannot leave the footprint. munlock fails only for a
		 * page that was never wired, which is what a failed commit leaves;
		 * releasing it is still right. */
		if (wire_)
			(void)munlock(address, length);
		if (madvise(address, length, MADV_FREE_REUSABLE) != 0) {
			const int error = errno;
			report_mapping_failure("guest RAM release", address, length,
				error);
			errno = error;
			return -1;
		}
		return 0;
	}
	if (madvise(address, length, MADV_FREE_REUSE) != 0) {
		const int error = errno;
		report_mapping_failure("guest RAM commit", address, length, error);
		errno = error;
		return -1;
	}
	if (wire_ && wire_host_range(address, length) != 0) {
		/* The page stays committed and usable, only compressible. */
		const int error = errno;
		wire_failures_.fetch_add(1, std::memory_order_relaxed);
		report_mapping_failure("guest RAM wire", address, length, error);
		errno = error;
		return -1;
	}
	return 0;
}

GuestRamCommitment *guest_ram_commitment() noexcept
{
	return published_commitment.load(std::memory_order_acquire);
}

void publish_guest_ram_commitment(
	std::unique_ptr<GuestRamCommitment> commitment) noexcept
{
	/* Guest RAM lives until the guest is handed back, and a thread that
	 * commits or releases only ever runs for a guest that still holds it. */
	GuestRamCommitment *expected = nullptr;
	if (published_commitment.compare_exchange_strong(expected,
		    commitment.get(), std::memory_order_acq_rel))
		(void)commitment.release();
}

void retire_guest_ram_commitment() noexcept
{
	delete published_commitment.exchange(nullptr, std::memory_order_acq_rel);
}

int release_guest_ram(std::uint64_t offset, std::uint64_t length) noexcept
{
	GuestRamCommitment *const commitment = guest_ram_commitment();
	if (commitment == nullptr) {
		errno = ENXIO;
		return -1;
	}
	return commitment->release(offset, length);
}

int commit_guest_ram(std::uint64_t offset, std::uint64_t length) noexcept
{
	GuestRamCommitment *const commitment = guest_ram_commitment();
	if (commitment == nullptr) {
		errno = ENXIO;
		return -1;
	}
	return commitment->commit(offset, length);
}

} // namespace uml::memory
