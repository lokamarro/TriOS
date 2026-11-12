// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_ram_image.hpp"

#include "platform/guest_timer_mask.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/semaphore.h>
#include <new>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>
#include <vector>

#include <compression.h>

namespace uml::memory {

namespace image_format {
namespace {

constexpr std::uint64_t k0 = 0xa0761d6478bd642fULL;
constexpr std::uint64_t k1 = 0xe7037ed1a0b428dbULL;
constexpr std::uint64_t k2 = 0x8ebc6af09c88c6e3ULL;
constexpr std::uint64_t k3 = 0x589965cc75374cc3ULL;
constexpr std::uint64_t k4 = 0x1d8e4e27c47d124fULL;

[[nodiscard]] inline std::uint64_t fold(std::uint64_t a,
	std::uint64_t b) noexcept
{
	const auto product = static_cast<unsigned __int128>(a) * b;
	return static_cast<std::uint64_t>(product) ^
		static_cast<std::uint64_t>(product >> 64);
}

[[nodiscard]] inline std::uint64_t load(const unsigned char *at) noexcept
{
	std::uint64_t value;
	std::memcpy(&value, at, sizeof value);
	return value;
}

} // namespace

std::uint64_t hash(const void *data, std::size_t size,
	std::uint64_t seed) noexcept
{
	const auto *at = static_cast<const unsigned char *>(data);
	std::uint64_t s0 = seed ^ k0;
	std::uint64_t s1 = seed ^ k1;
	std::uint64_t s2 = seed ^ k2;
	std::uint64_t s3 = seed ^ k3;
	std::size_t left = size;
	const auto step = [&](const unsigned char *block) noexcept {
		s0 = fold(load(block) ^ k1, load(block + 8) ^ s0);
		s1 = fold(load(block + 16) ^ k2, load(block + 24) ^ s1);
		s2 = fold(load(block + 32) ^ k3, load(block + 40) ^ s2);
		s3 = fold(load(block + 48) ^ k4, load(block + 56) ^ s3);
	};
	for (; left >= 64; left -= 64, at += 64)
		step(at);
	if (left != 0) {
		unsigned char tail[64] = {};
		std::memcpy(tail, at, left);
		step(tail);
	}
	return fold(s0 ^ k1, fold(s1 ^ s2 ^ k2, s3 ^ k3) ^ (size ^ k4));
}

} // namespace image_format

namespace {

using image_format::Entry;
using image_format::Header;
using image_format::Kind;

constexpr std::size_t chunk_pages = image_format::chunk_pages;
constexpr std::string_view image_prefix{"uml-hibernation-"};
constexpr std::string_view image_suffix{".img"};

[[nodiscard]] bool all_zero(const unsigned char *data,
	std::size_t size) noexcept
{
	constexpr std::size_t block = 4096;
	for (std::size_t at = 0; at < size; at += block) {
		const std::size_t length = std::min(block, size - at);
		std::uint64_t seen = 0;
		for (std::size_t word = 0; word + 8 <= length; word += 8) {
			std::uint64_t value;
			std::memcpy(&value, data + at + word, sizeof value);
			seen |= value;
		}
		if (seen != 0)
			return false;
	}
	return true;
}

[[nodiscard]] bool write_all(int fd, const void *data, std::size_t size,
	std::uint64_t offset) noexcept
{
	const auto *at = static_cast<const unsigned char *>(data);
	while (size != 0) {
		const ssize_t written = pwrite(fd, at, size,
			static_cast<off_t>(offset));
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0) {
			if (written == 0)
				errno = EIO;
			return false;
		}
		at += written;
		offset += static_cast<std::uint64_t>(written);
		size -= static_cast<std::size_t>(written);
	}
	return true;
}

[[nodiscard]] bool read_all(int fd, void *data, std::size_t size,
	std::uint64_t offset) noexcept
{
	auto *at = static_cast<unsigned char *>(data);
	while (size != 0) {
		const ssize_t got = pread(fd, at, size, static_cast<off_t>(offset));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0) {
			if (got == 0)
				errno = EIO;
			return false;
		}
		at += got;
		offset += static_cast<std::uint64_t>(got);
		size -= static_cast<std::size_t>(got);
	}
	return true;
}

/* Calls f(first_bit, bits) for each run of set bits in the mask. */
template <typename Function>
void for_each_run(std::uint64_t mask, Function &&f) noexcept
{
	while (mask != 0) {
		const auto first = static_cast<std::size_t>(std::countr_zero(mask));
		const auto bits = static_cast<std::size_t>(
			std::countr_one(mask >> first));
		f(first, bits);
		mask = first + bits >= 64 ? 0 :
			mask & ~(((std::uint64_t{1} << bits) - 1) << first);
	}
}

[[nodiscard]] bool single_run(std::uint64_t mask, std::size_t &first,
	std::size_t &bits) noexcept
{
	first = static_cast<std::size_t>(std::countr_zero(mask));
	bits = static_cast<std::size_t>(std::countr_one(mask >> first));
	return first + bits >= 64 ||
		(mask >> (first + bits)) == 0;
}

/* Mach semaphores wake a waiting thread with a trap and no user-space lock, so
 * the pool works while threads that might hold the allocator are stopped. */
void wait(semaphore_t semaphore) noexcept
{
	kern_return_t result;
	do {
		result = semaphore_wait(semaphore);
	} while (result == KERN_ABORTED);
}

class Workers final {
public:
	using Function = void (*)(void *context, unsigned worker);

	Workers() = default;
	Workers(const Workers &) = delete;
	Workers &operator=(const Workers &) = delete;
	~Workers() { stop(); }

	/* `total` includes the calling thread, which is worker 0. */
	[[nodiscard]] bool start(unsigned total) noexcept
	{
		total_ = std::max(1U, total);
		if (semaphore_create(mach_task_self(), &done_, SYNC_POLICY_FIFO, 0) !=
			KERN_SUCCESS) {
			errno = ENOMEM;
			return false;
		}
		done_created_ = true;
		try {
			slots_.resize(total_ - 1);
		} catch (...) {
			errno = ENOMEM;
			return false;
		}
		for (unsigned index = 0; index + 1 < total_; ++index) {
			Slot &slot = slots_[index];
			slot.owner = this;
			slot.worker = index + 1;
			if (semaphore_create(mach_task_self(), &slot.wake,
				    SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) {
				errno = ENOMEM;
				return false;
			}
			slot.wake_created = true;
			pthread_attr_t attributes;
			if (pthread_attr_init(&attributes) != 0) {
				errno = ENOMEM;
				return false;
			}
			(void)pthread_attr_set_qos_class_np(&attributes,
				QOS_CLASS_USER_INITIATED, 0);
			int error;
			{
				/* The guest's clock signal must never land on a worker. */
				const platform::ScopedGuestTimerBlock block{
					"hibernation worker"};
				error = pthread_create(&slot.thread, &attributes, &entry,
					&slot);
			}
			pthread_attr_destroy(&attributes);
			if (error != 0) {
				errno = error;
				return false;
			}
			slot.started = true;
		}
		return true;
	}

	void stop() noexcept
	{
		stopping_.store(true, std::memory_order_release);
		for (Slot &slot : slots_) {
			if (slot.started) {
				(void)semaphore_signal(slot.wake);
				(void)pthread_join(slot.thread, nullptr);
				slot.started = false;
			}
			if (slot.wake_created) {
				(void)semaphore_destroy(mach_task_self(), slot.wake);
				slot.wake_created = false;
			}
		}
		if (done_created_) {
			(void)semaphore_destroy(mach_task_self(), done_);
			done_created_ = false;
		}
	}

	[[nodiscard]] unsigned total() const noexcept { return total_; }

	/* Runs function on every worker and returns when all have finished. */
	void run(Function function, void *context) noexcept
	{
		function_ = function;
		context_ = context;
		for (unsigned index = 0; index + 1 < total_; ++index)
			(void)semaphore_signal(slots_[index].wake);
		function(context, 0);
		for (unsigned index = 0; index + 1 < total_; ++index)
			wait(done_);
	}

private:
	struct Slot final {
		Workers *owner{};
		pthread_t thread{};
		semaphore_t wake{};
		unsigned worker{};
		bool wake_created{};
		bool started{};
	};

	static void *entry(void *argument) noexcept
	{
		auto *slot = static_cast<Slot *>(argument);
		(void)pthread_setname_np("hibernate-worker");
		Workers &pool = *slot->owner;
		for (;;) {
			wait(slot->wake);
			if (pool.stopping_.load(std::memory_order_acquire))
				return nullptr;
			pool.function_(pool.context_, slot->worker);
			(void)semaphore_signal(pool.done_);
		}
	}

	std::vector<Slot> slots_;
	semaphore_t done_{};
	bool done_created_{};
	unsigned total_{1};
	Function function_{};
	void *context_{};
	std::atomic<bool> stopping_{false};
};

/* The memory one worker needs for one pass: a gather buffer for chunks whose
 * pages are not adjacent, a buffer for the stored form, and the codec's
 * scratch. All of it is mapped, not allocated, and gone when the pass ends. */
class PassMemory final {
public:
	PassMemory() = default;
	PassMemory(const PassMemory &) = delete;
	PassMemory &operator=(const PassMemory &) = delete;
	~PassMemory()
	{
		if (base_ != nullptr)
			(void)munmap(base_, size_);
	}

	[[nodiscard]] bool map(std::size_t chunk_bytes,
		std::size_t codec_bytes) noexcept
	{
		codec_bytes = image_format::round_up(codec_bytes, 16384);
		size_ = 2 * chunk_bytes + codec_bytes;
		void *mapping = mmap(nullptr, size_, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANON, -1, 0);
		if (mapping == MAP_FAILED)
			return false;
		base_ = static_cast<unsigned char *>(mapping);
		gather = base_;
		stored = base_ + chunk_bytes;
		codec = codec_bytes != 0 ? base_ + 2 * chunk_bytes : nullptr;
		return true;
	}

	unsigned char *gather{};
	unsigned char *stored{};
	void *codec{};

private:
	unsigned char *base_{};
	std::size_t size_{};
};

} // namespace

class GuestRamImage::State final {
public:
	State(GuestRamCommitment &ram, const Options &options)
		: ram_(ram), options_(options) {}

	~State()
	{
		workers_.stop();
		if (fd_ >= 0)
			(void)close(fd_);
	}

	[[nodiscard]] bool open()
	{
		page_size_ = ram_.page_size();
		chunk_bytes_ = chunk_pages * page_size_;
		const std::size_t first_managed =
			(ram_.managed_begin() + page_size_ - 1) / page_size_;
		first_word_ = first_managed / chunk_pages;
		if (ram_.word_count() <= first_word_) {
			errno = EINVAL;
			return false;
		}
		chunk_count_ = ram_.word_count() - first_word_;
		data_offset_ = image_format::data_offset(chunk_count_);
		table_.assign(chunk_count_, Entry{});
		for (Entry &entry : table_)
			entry.kind = Kind::invalid;
		file_table_.resize(chunk_count_);
		evicted_.assign(chunk_count_, 0);
		encode_scratch_ = compression_encode_scratch_buffer_size(
			COMPRESSION_LZ4);
		decode_scratch_ = compression_decode_scratch_buffer_size(
			COMPRESSION_LZ4);
		worker_stats_.resize(std::max(1U, options_.threads));

		fd_ = ::open(options_.path.c_str(),
			O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd_ < 0)
			return false;
		/* A process suspended with a file lock is killed by iOS
		 * (0xdead10cc). This image belongs only to this process, so an
		 * unlinked file needs neither a lock nor stale-file cleanup. */
		if (unlink(options_.path.c_str()) != 0)
			return false;
		if (options_.bypass_cache)
			(void)fcntl(fd_, F_NOCACHE, 1);
		const std::uint64_t file_size =
			data_offset_ + chunk_count_ * chunk_bytes_;
		if (ftruncate(fd_, static_cast<off_t>(file_size)) != 0)
			return false;
		if (!workers_.start(std::max(1U, options_.threads)))
			return false;
		return true;
	}

	[[nodiscard]] bool freeze() noexcept
	{
		ram_.hold_changes();
		if (ram_.changes_in_flight() != 0) {
			ram_.allow_changes();
			return false;
		}
		frozen_ = true;
		return true;
	}

	[[nodiscard]] bool hibernate(ImageStats &stats) noexcept
	{
		stats = {};
		if (!frozen_) {
			stats.error = EPERM;
			return false;
		}
		if (evicted_chunks() != 0)
			return true;
		for (ImageStats &each : worker_stats_)
			each = {};
		failed_.store(false, std::memory_order_relaxed);
		next_.store(0, std::memory_order_relaxed);
		workers_.run(&State::hibernate_entry, this);
		for (const ImageStats &each : worker_stats_)
			merge(stats, each);
		/* Chunks no worker reached, because none could map its buffers, stay
		 * in the process like those an abort leaves. */
		const std::size_t reached = std::min(
			next_.load(std::memory_order_relaxed), chunk_count_);
		stats.chunks_skipped += chunk_count_ - reached;
		const bool persisted = persist(stats);
		if (!persisted && stats.error == 0)
			stats.error = errno;
		for (const Entry &entry : table_)
			stats.stored_bytes += entry.stored;
		return stats.chunks_failed == 0 && stats.chunks_skipped == 0;
	}

	[[nodiscard]] bool restore(ImageStats &stats) noexcept
	{
		stats = {};
		const bool outside = evicted_chunks() != 0;
		bool intact = true;
		if (outside) {
			if (table_persisted_ && !check_file(stats)) {
				intact = false;
			} else {
				for (ImageStats &each : worker_stats_)
					each = {};
				failed_.store(false, std::memory_order_relaxed);
				next_.store(0, std::memory_order_relaxed);
				wire_failures_before_ = ram_.wire_failures();
				workers_.run(&State::restore_entry, this);
				for (const ImageStats &each : worker_stats_)
					merge(stats, each);
				/* A chunk no worker reached is still out of the process. */
				const std::size_t reached = std::min(
					next_.load(std::memory_order_relaxed), chunk_count_);
				for (std::size_t chunk = reached; chunk < chunk_count_;
				     ++chunk)
					stats.chunks_failed += evicted_[chunk];
				stats.wire_failures =
					ram_.wire_failures() - wire_failures_before_;
				intact = stats.chunks_failed == 0;
			}
		}
		for (const Entry &entry : table_)
			stats.stored_bytes += entry.stored;
		if (intact && frozen_) {
			/* Every page is back: the guest may change what it holds. */
			ram_.allow_changes();
			frozen_ = false;
		}
		return intact;
	}

	[[nodiscard]] std::size_t evicted_chunks() const noexcept
	{
		std::size_t count = 0;
		for (const std::uint8_t flag : evicted_)
			count += flag;
		return count;
	}

	void request_abort() noexcept
	{
		abort_.store(true, std::memory_order_release);
	}
	void clear_abort() noexcept
	{
		abort_.store(false, std::memory_order_release);
	}

	[[nodiscard]] std::size_t chunk_bytes() const noexcept
	{
		return chunk_bytes_;
	}
	[[nodiscard]] std::size_t chunk_count() const noexcept
	{
		return chunk_count_;
	}

private:
	static void merge(ImageStats &total, const ImageStats &each) noexcept
	{
		total.chunks_held += each.chunks_held;
		total.chunks_written += each.chunks_written;
		total.chunks_kept += each.chunks_kept;
		total.chunks_zero += each.chunks_zero;
		total.chunks_failed += each.chunks_failed;
		total.chunks_skipped += each.chunks_skipped;
		total.evicted_bytes += each.evicted_bytes;
		total.written_bytes += each.written_bytes;
		total.read_bytes += each.read_bytes;
		total.image_bytes += each.image_bytes;
		if (total.error == 0)
			total.error = each.error;
	}

	[[nodiscard]] std::uint64_t slot_offset(std::size_t index) const noexcept
	{
		return data_offset_ + index * chunk_bytes_;
	}

	[[nodiscard]] unsigned char *page_address(std::size_t chunk,
		std::size_t bit) const noexcept
	{
		return reinterpret_cast<unsigned char *>(ram_.root()) +
			((first_word_ + chunk) * chunk_pages + bit) * page_size_;
	}

	[[nodiscard]] std::size_t first_page(std::size_t chunk,
		std::size_t bit) const noexcept
	{
		return (first_word_ + chunk) * chunk_pages + bit;
	}

	void gather(std::size_t chunk, std::uint64_t mask,
		unsigned char *into) const noexcept
	{
		std::size_t at = 0;
		for_each_run(mask, [&](std::size_t first, std::size_t bits) {
			std::memcpy(into + at, page_address(chunk, first),
				bits * page_size_);
			at += bits * page_size_;
		});
	}

	void scatter(std::size_t chunk, std::uint64_t mask,
		const unsigned char *from) const noexcept
	{
		std::size_t at = 0;
		for_each_run(mask, [&](std::size_t first, std::size_t bits) {
			std::memcpy(page_address(chunk, first), from + at,
				bits * page_size_);
			at += bits * page_size_;
		});
	}

	static void hibernate_entry(void *context, unsigned worker) noexcept
	{
		static_cast<State *>(context)->hibernate_pass(worker);
	}

	static void restore_entry(void *context, unsigned worker) noexcept
	{
		static_cast<State *>(context)->restore_pass(worker);
	}

	void note_failure(ImageStats &stats, int error) noexcept
	{
		++stats.chunks_failed;
		if (stats.error == 0)
			stats.error = error;
		failed_.store(true, std::memory_order_relaxed);
	}

	void hibernate_pass(unsigned worker) noexcept
	{
		ImageStats &stats = worker_stats_[worker];
		PassMemory memory;
		if (!memory.map(chunk_bytes_,
			    std::max(encode_scratch_, decode_scratch_))) {
			/* Without buffers this worker stores nothing; the others
			 * carry on, and what nobody reaches stays resident. */
			stats.error = stats.error == 0 ? ENOMEM : stats.error;
			return;
		}
		for (;;) {
			const std::size_t chunk =
				next_.fetch_add(1, std::memory_order_relaxed);
			if (chunk >= chunk_count_)
				return;
			hibernate_chunk(chunk, memory, stats);
		}
	}

	void hibernate_chunk(std::size_t chunk, PassMemory &memory,
		ImageStats &stats) noexcept
	{
		Entry &entry = table_[chunk];
		const std::uint64_t mask = ram_.held_word(first_word_ + chunk);
		if (mask == 0) {
			entry = Entry{};
			entry.kind = Kind::empty;
			evicted_[chunk] = 0;
			return;
		}
		++stats.chunks_held;
		if (abort_.load(std::memory_order_acquire)) {
			++stats.chunks_skipped;
			return;
		}
		const std::size_t pages = static_cast<std::size_t>(
			std::popcount(mask));
		const std::size_t length = pages * page_size_;
		std::size_t first = 0, bits = 0;
		const unsigned char *data;
		if (single_run(mask, first, bits)) {
			data = page_address(chunk, first);
		} else {
			gather(chunk, mask, memory.gather);
			data = memory.gather;
		}
		const std::uint64_t digest = image_format::hash(data, length);
		const bool unchanged = entry.kind != Kind::invalid &&
			entry.kind != Kind::empty && entry.mask == mask &&
			entry.hash == digest;
		if (unchanged) {
			++stats.chunks_kept;
		} else if (!store(chunk, mask, data, length, digest, memory,
			    stats)) {
			return;
		}
		stats.image_bytes += length;
		if (entry.kind == Kind::zero)
			++stats.chunks_zero;
		bool evict_failed = false;
		for_each_run(mask, [&](std::size_t run_first, std::size_t run_bits) {
			if (ram_.evict(first_page(chunk, run_first), run_bits) != 0)
				evict_failed = true;
		});
		/* A chunk whose eviction half failed is in the image either way, and
		 * the restore puts every page of it back. */
		evicted_[chunk] = 1;
		stats.evicted_bytes += length;
		if (evict_failed)
			note_failure(stats, errno);
	}

	/* Writes the chunk's pages to its slot and records them. On failure the
	 * chunk's pages stay in the process and its entry says the slot is
	 * unknown. */
	[[nodiscard]] bool store(std::size_t chunk, std::uint64_t mask,
		const unsigned char *data, std::size_t length,
		std::uint64_t digest, PassMemory &memory,
		ImageStats &stats) noexcept
	{
		Entry &entry = table_[chunk];
		entry = Entry{};
		entry.kind = Kind::invalid;
		Kind kind = Kind::raw;
		std::size_t size = length;
		const unsigned char *source = data;
		if (all_zero(data, length)) {
			kind = Kind::zero;
			size = 0;
		} else if (options_.compress) {
			const std::size_t limit = length - length / 16;
			const std::size_t packed = compression_encode_buffer(
				memory.stored, limit, data, length, memory.codec,
				COMPRESSION_LZ4);
			if (packed != 0 && packed < limit) {
				kind = Kind::lz4;
				size = packed;
				source = memory.stored;
			}
		}
		if (size != 0) {
			if (!write_all(fd_, source, size, slot_offset(chunk))) {
				note_failure(stats, errno);
				return false;
			}
			stats.written_bytes += size;
		}
		entry.mask = mask;
		entry.hash = digest;
		entry.stored = static_cast<std::uint32_t>(size);
		entry.kind = kind;
		++stats.chunks_written;
		return true;
	}

	/* The header and the table, written once every chunk has been stored. */
	[[nodiscard]] bool persist(ImageStats &stats) noexcept
	{
		Header header{};
		std::memcpy(header.magic, image_format::magic.data(),
			sizeof header.magic);
		header.version = image_format::version;
		header.header_size = sizeof(Header);
		header.page_size = static_cast<std::uint32_t>(page_size_);
		header.chunk_pages = chunk_pages;
		header.chunk_count = chunk_count_;
		header.first_page = first_word_ * chunk_pages;
		header.ram_bytes = ram_.size();
		header.generation = ++generation_;
		const std::size_t table_bytes = chunk_count_ * sizeof(Entry);
		header.table_hash = image_format::hash(table_.data(), table_bytes);
		header.header_hash = image_format::hash(&header,
			offsetof(Header, header_hash));
		table_persisted_ = false;
		if (!write_all(fd_, table_.data(), table_bytes,
			    image_format::table_offset()) ||
		    !write_all(fd_, &header, sizeof header, 0))
			return false;
		table_persisted_ = true;
		persisted_header_ = header;
		stats.written_bytes += table_bytes + sizeof header;
		return true;
	}

	/* The file's header and table against what this process wrote. */
	[[nodiscard]] bool check_file(ImageStats &stats) noexcept
	{
		Header header{};
		const std::size_t table_bytes = chunk_count_ * sizeof(Entry);
		if (!read_all(fd_, &header, sizeof header, 0) ||
		    !read_all(fd_, file_table_.data(), table_bytes,
			    image_format::table_offset())) {
			stats.error = errno;
			return false;
		}
		stats.read_bytes += table_bytes + sizeof header;
		const bool header_ok = std::memcmp(&header, &persisted_header_,
			sizeof header) == 0 &&
			header.header_hash == image_format::hash(&header,
				offsetof(Header, header_hash)) &&
			header.table_hash == image_format::hash(file_table_.data(),
				table_bytes);
		if (!header_ok ||
		    std::memcmp(file_table_.data(), table_.data(), table_bytes) != 0) {
			stats.error = EILSEQ;
			return false;
		}
		return true;
	}

	void restore_pass(unsigned worker) noexcept
	{
		ImageStats &stats = worker_stats_[worker];
		PassMemory memory;
		if (!memory.map(chunk_bytes_,
			    std::max(encode_scratch_, decode_scratch_))) {
			/* The chunks this worker would have taken are left to the
			 * others; one that nobody takes is reported below. */
			stats.error = stats.error == 0 ? ENOMEM : stats.error;
			return;
		}
		for (;;) {
			const std::size_t chunk =
				next_.fetch_add(1, std::memory_order_relaxed);
			if (chunk >= chunk_count_)
				return;
			if (evicted_[chunk] != 0)
				restore_chunk(chunk, memory, stats);
		}
	}

	void restore_chunk(std::size_t chunk, PassMemory &memory,
		ImageStats &stats) noexcept
	{
		const Entry entry = table_[chunk];
		const std::uint64_t mask = entry.mask;
		const std::size_t length = static_cast<std::size_t>(
			std::popcount(mask)) * page_size_;
		if (mask == 0 || (entry.kind != Kind::zero &&
			    entry.kind != Kind::raw && entry.kind != Kind::lz4)) {
			note_failure(stats, EILSEQ);
			return;
		}
		/* Counted again and wired before a byte is written, so that nothing
		 * restored can be compressed or dropped before the guest runs. */
		bool readmitted = true;
		for_each_run(mask, [&](std::size_t first, std::size_t bits) {
			if (ram_.readmit(first_page(chunk, first), bits) != 0)
				readmitted = false;
		});
		if (!readmitted) {
			note_failure(stats, errno);
			return;
		}
		std::size_t first = 0, bits = 0;
		const bool contiguous = single_run(mask, first, bits);
		unsigned char *const target = contiguous ?
			page_address(chunk, first) : memory.gather;
		bool good = true;
		int error = EILSEQ;
		if (entry.kind == Kind::zero) {
			std::memset(target, 0, length);
		} else if (entry.kind == Kind::raw) {
			if (entry.stored != length ||
			    !read_all(fd_, target, length, slot_offset(chunk))) {
				good = false;
				error = entry.stored != length ? EILSEQ : errno;
			} else {
				stats.read_bytes += length;
			}
		} else {
			if (entry.stored == 0 || entry.stored > length ||
			    !read_all(fd_, memory.stored, entry.stored,
				    slot_offset(chunk))) {
				good = false;
				error = entry.stored == 0 || entry.stored > length ?
					EILSEQ : errno;
			} else {
				stats.read_bytes += entry.stored;
				good = compression_decode_buffer(target, length,
					memory.stored, entry.stored, memory.codec,
					COMPRESSION_LZ4) == length;
			}
		}
		if (good && entry.kind != Kind::zero)
			good = image_format::hash(target, length) == entry.hash;
		if (!good) {
			note_failure(stats, error);
			return;
		}
		if (!contiguous)
			scatter(chunk, mask, memory.gather);
		evicted_[chunk] = 0;
		stats.evicted_bytes += length;
		++stats.chunks_held;
	}

	GuestRamCommitment &ram_;
	Options options_;
	int fd_{-1};
	std::size_t page_size_{};
	std::size_t chunk_bytes_{};
	std::size_t chunk_count_{};
	std::size_t first_word_{};
	std::uint64_t data_offset_{};
	std::size_t encode_scratch_{};
	std::size_t decode_scratch_{};
	/* What the image holds, chunk by chunk. */
	std::vector<Entry> table_;
	/* The table as read back at a restore. */
	std::vector<Entry> file_table_;
	/* Chunks whose pages are out of the process. */
	std::vector<std::uint8_t> evicted_;
	std::vector<ImageStats> worker_stats_;
	Workers workers_;
	std::atomic<std::size_t> next_{0};
	std::atomic<bool> failed_{false};
	std::atomic<bool> abort_{false};
	bool frozen_{false};
	bool table_persisted_{false};
	std::uint64_t generation_{0};
	Header persisted_header_{};
	std::uint64_t wire_failures_before_{0};
};

std::unique_ptr<GuestRamImage> GuestRamImage::create(
	GuestRamCommitment &commitment, const Options &options) noexcept
{
	try {
		auto state = std::make_unique<State>(commitment, options);
		if (!state->open()) {
			/* Tearing the half-built state down must not replace why. */
			const int error = errno;
			state.reset();
			errno = error;
			return nullptr;
		}
		return std::unique_ptr<GuestRamImage>{
			new GuestRamImage(std::move(state))};
	} catch (const std::bad_alloc &) {
		errno = ENOMEM;
	} catch (...) {
		errno = EIO;
	}
	return nullptr;
}

GuestRamImage::GuestRamImage(std::unique_ptr<State> state) noexcept
	: state_(std::move(state))
{
}

GuestRamImage::~GuestRamImage() = default;

bool GuestRamImage::freeze_changes() noexcept
{
	return state_->freeze();
}

bool GuestRamImage::hibernate(ImageStats &stats) noexcept
{
	return state_->hibernate(stats);
}

bool GuestRamImage::restore(ImageStats &stats) noexcept
{
	return state_->restore(stats);
}

bool GuestRamImage::evicted() const noexcept
{
	return state_->evicted_chunks() != 0;
}

void GuestRamImage::request_abort() noexcept
{
	state_->request_abort();
}

void GuestRamImage::clear_abort() noexcept
{
	state_->clear_abort();
}

std::size_t GuestRamImage::chunk_bytes() const noexcept
{
	return state_->chunk_bytes();
}

std::size_t GuestRamImage::chunk_count() const noexcept
{
	return state_->chunk_count();
}

std::string guest_ram_image_name(int pid)
{
	return std::string{image_prefix} + std::to_string(pid) +
		std::string{image_suffix};
}

std::size_t remove_stale_guest_ram_images(const std::string &directory) noexcept
{
	std::size_t removed = 0;
	try {
		DIR *const listing = opendir(directory.c_str());
		if (listing == nullptr)
			return 0;
		std::vector<std::string> names;
		while (const dirent *entry = readdir(listing)) {
			const std::string_view name{entry->d_name};
			if (name.size() > image_prefix.size() + image_suffix.size() &&
			    name.starts_with(image_prefix) &&
			    name.ends_with(image_suffix))
				names.emplace_back(name);
		}
		(void)closedir(listing);
		for (const std::string &name : names) {
			const std::string path = directory + "/" + name;
			const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
			if (fd < 0)
				continue;
			/* A live image of an older version holds its process's lock. */
			if (flock(fd, LOCK_EX | LOCK_NB) == 0 &&
			    unlink(path.c_str()) == 0)
				++removed;
			(void)::close(fd);
		}
	} catch (...) {
	}
	return removed;
}

} // namespace uml::memory
