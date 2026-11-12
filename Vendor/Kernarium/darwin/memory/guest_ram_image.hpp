// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "guest_ram_commitment.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace uml::memory {

/* An image of the guest RAM the guest holds, kept in a file so that the RAM
 * itself can leave the process while the guest is paused.
 *
 * The held pages are cut into chunks of 64 host pages, which makes a chunk's
 * state one word of the commitment's bitmap. A chunk is stored as its held
 * pages one after the other, either raw or compressed with LZ4, in a slot of
 * its own, so a chunk is rewritten in place and the file never needs
 * compacting. The file is sparse: only the stored bytes reach flash. A header
 * and a table, one entry per chunk, record what the slots hold, each guarded
 * by a hash of the data it describes.
 *
 * `hibernate` stores every chunk that differs from what the image already
 * holds, which is all of them the first time, and then evicts every chunk from
 * the process (`GuestRamCommitment::evict`). `restore` brings each evicted
 * chunk back, checks it against the image's own hash and wires it again. The
 * image describes one process's guest: it is never read by another process,
 * and a file left by one that was killed is deleted, not restored.
 *
 * Neither call allocates from the heap or takes a lock, and neither writes to
 * a stdio stream, because the caller holds every thread that could be inside
 * the allocator or a stream: the buffers are mapped with mmap and the workers
 * exist from the start.
 *
 * `hibernate` holds the commitment's changes and `restore` allows them again
 * once the last chunk is back. The caller owns the order around them: stop
 * every thread that touches guest RAM first, and let them go only after
 * `restore`. */
namespace image_format {

inline constexpr std::string_view magic{"UMLHIB01", 8};
inline constexpr std::uint32_t version = 1;
/* Pages in a chunk: the commitment's bitmap word. */
inline constexpr std::size_t chunk_pages = 64;
/* The header and the table each start on a 16 KiB boundary, and so does the
 * first slot. */
inline constexpr std::size_t metadata_alignment = 16384;

enum class Kind : std::uint8_t {
	/* The chunk holds no pages. */
	empty = 0,
	/* Every held page is zero: nothing is stored. */
	zero = 1,
	/* The held pages as they are. */
	raw = 2,
	/* The held pages as Apple's LZ4. */
	lz4 = 3,
	/* The slot's content is unknown: a store did not finish. Never restored. */
	invalid = 255,
};

struct Header final {
	char magic[8];
	std::uint32_t version;
	std::uint32_t header_size;
	std::uint32_t page_size;
	std::uint32_t chunk_pages;
	std::uint64_t chunk_count;
	/* Index of the first page of chunk 0, a multiple of chunk_pages. */
	std::uint64_t first_page;
	std::uint64_t ram_bytes;
	/* Counts the hibernations that completed. */
	std::uint64_t generation;
	/* Hash of the table as written. */
	std::uint64_t table_hash;
	/* Hash of every byte of the header before this one. */
	std::uint64_t header_hash;
};

struct Entry final {
	/* Bit i: page i of the chunk is stored. */
	std::uint64_t mask;
	/* Hash of the stored pages, in page order. */
	std::uint64_t hash;
	/* Bytes in the slot, 0 for a zero or empty chunk. */
	std::uint32_t stored;
	Kind kind;
	std::uint8_t reserved[11];
};
static_assert(sizeof(Header) == 72);
static_assert(sizeof(Entry) == 32);

[[nodiscard]] constexpr std::size_t round_up(std::size_t value,
	std::size_t unit) noexcept
{
	return (value + unit - 1) / unit * unit;
}

/* The file offset of the table, and of the slot of chunk `index`. */
[[nodiscard]] constexpr std::uint64_t table_offset() noexcept
{
	return metadata_alignment;
}
[[nodiscard]] constexpr std::uint64_t data_offset(
	std::size_t chunk_count) noexcept
{
	return metadata_alignment + round_up(chunk_count * sizeof(Entry),
		metadata_alignment);
}

/* The 64-bit hash that guards every chunk and the metadata: eight bytes per
 * step over four independent lanes, so that it runs at memory speed. `size`
 * is any length; the lanes see a final partial block zero-padded. */
[[nodiscard]] std::uint64_t hash(const void *data, std::size_t size,
	std::uint64_t seed = 0) noexcept;

} // namespace image_format

struct ImageStats final {
	/* Chunks that held pages when the image was taken, and what became of
	 * them: stored again, found unchanged, or all zero. */
	std::uint64_t chunks_held{};
	std::uint64_t chunks_written{};
	std::uint64_t chunks_kept{};
	std::uint64_t chunks_zero{};
	/* Chunks that could not be stored, restored or verified. A chunk that
	 * could not be stored stays in the process. */
	std::uint64_t chunks_failed{};
	/* Chunks left in the process because an abort was requested. */
	std::uint64_t chunks_skipped{};
	/* Bytes of guest RAM that left the process, or came back. */
	std::uint64_t evicted_bytes{};
	/* Bytes this call wrote to the file (data and metadata) and read from it. */
	std::uint64_t written_bytes{};
	std::uint64_t read_bytes{};
	/* Data bytes the image holds now, and the bytes of guest RAM they stand
	 * for. */
	std::uint64_t stored_bytes{};
	std::uint64_t image_bytes{};
	/* Pages that could not be wired as they came back. */
	std::uint64_t wire_failures{};
	/* errno of the first failure, 0 when none. */
	int error{};
};

class GuestRamImage final {
public:
	struct Options final {
		/* Where the image file is created, exclusively, and unlinked at once
		 * with no file lock: a process suspended with a lock is ended by iOS.
		 * The descriptor keeps its contents alive until closed, including
		 * across suspension, and the OS reclaims them if the process ends,
		 * so the path names nothing once `create` returns. */
		std::string path;
		/* Threads that store and restore chunks, the caller's included. */
		unsigned threads{4};
		/* Compress chunks; otherwise store them raw. */
		bool compress{true};
		/* Keep the image out of the host's file cache, which would only
		 * hold the guest's memory a second time. */
		bool bypass_cache{true};
	};

	/* Null with errno set when the file, the table or a worker cannot be
	 * had. Allocates; call it before any thread is held. */
	[[nodiscard]] static std::unique_ptr<GuestRamImage> create(
		GuestRamCommitment &commitment, const Options &options) noexcept;
	~GuestRamImage();
	GuestRamImage(const GuestRamImage &) = delete;
	GuestRamImage &operator=(const GuestRamImage &) = delete;

	/* Holds the commitment's changes and checks that none is in flight. False,
	 * with changes allowed again, when one is: a thread is stopped inside a
	 * commit or release, so release the threads, let it finish and try again.
	 * Call it with every thread that can commit or release held. Whatever
	 * follows, `restore` ends the freeze. */
	[[nodiscard]] bool freeze_changes() noexcept;
	/* Stores what changed and evicts every chunk the guest holds. False when
	 * any chunk stayed in the process; the chunks that left are still
	 * restored by `restore`. Needs `freeze_changes` to have succeeded. */
	[[nodiscard]] bool hibernate(ImageStats &stats) noexcept;
	/* Brings every evicted chunk back, verified and wired, then lets the
	 * commitment change again. False when a chunk could not be read or did not
	 * match its hash: the guest's memory is then incomplete and the guest must
	 * not run. */
	[[nodiscard]] bool restore(ImageStats &stats) noexcept;

	/* Whether any chunk is out of the process. */
	[[nodiscard]] bool evicted() const noexcept;
	/* Makes a running `hibernate` leave the chunks it has not reached in the
	 * process. Safe from any thread. */
	void request_abort() noexcept;
	void clear_abort() noexcept;

	/* The size of a chunk in bytes. */
	[[nodiscard]] std::size_t chunk_bytes() const noexcept;
	[[nodiscard]] std::size_t chunk_count() const noexcept;

private:
	class State;
	explicit GuestRamImage(std::unique_ptr<State> state) noexcept;

	std::unique_ptr<State> state_;
};

/* Deletes the images that a process which has ended left in `directory`:
 * files named uml-hibernation-<pid>.img whose lock is free, as older versions
 * left them (an image now has no name). Returns how many it removed. */
std::size_t remove_stale_guest_ram_images(
	const std::string &directory) noexcept;

/* The file name the image of process `pid` has. */
[[nodiscard]] std::string guest_ram_image_name(int pid);

} // namespace uml::memory
