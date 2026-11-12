// SPDX-License-Identifier: GPL-2.0-or-later
/* The guest RAM image against real anonymous memory: what it stores, what it
 * leaves out, that a restore returns every held page exactly and wired, that
 * a second image rewrites only what changed, and that a damaged or short
 * image fails the restore instead of returning wrong memory. */
#include "mach_vm_region_test_support.hpp"
#include "memory/guest_ram_commitment.hpp"
#include "memory/guest_ram_image.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <exception>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <mach/mach.h>
#include <set>
#include <string>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using uml::memory::GuestRamCommitment;
using uml::memory::GuestRamImage;
using uml::memory::ImageStats;
using uml::testing::host_page;
using uml::testing::require;
using uml::testing::task_footprint;
using uml::testing::wired_count;
namespace format = uml::memory::image_format;

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UML_IMAGE_TEST_UNDER_ASAN 1
#endif
#endif

/* AddressSanitizer intercepts mlock() and does nothing, so no page is ever
 * wired and the footprint checks that depend on wiring have nothing to see. */
#if defined(UML_IMAGE_TEST_UNDER_ASAN)
constexpr bool wiring_works = false;
#else
constexpr bool wiring_works = true;
#endif

constexpr std::size_t mib = 1024U * 1024U;

[[nodiscard]] std::uint64_t splitmix(std::uint64_t &state) noexcept
{
	std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

enum class Fill { random, repeating, zero };

/* The content a page of the test RAM has, so that a restore can be checked
 * against what was there, byte for byte. */
void fill_page(std::byte *page, std::size_t size, std::size_t index,
	Fill kind, std::uint64_t salt = 0)
{
	std::uint64_t state = index * 0x1234567ULL + salt;
	auto *words = reinterpret_cast<std::uint64_t *>(page);
	for (std::size_t word = 0; word < size / 8; ++word) {
		switch (kind) {
		case Fill::random:
			words[word] = splitmix(state);
			break;
		case Fill::repeating:
			words[word] = 0x0101010101010101ULL * ((index + salt) & 0xff) +
				(word & 7);
			break;
		case Fill::zero:
			words[word] = 0;
			break;
		}
	}
}

/* Test RAM with a partial first chunk, a partial last chunk, and in between
 * chunks of every kind a stored chunk can be:
 *
 *   chunk 0  44 pages (the first 20 of the chunk are not managed), repeating
 *   chunk 1  64 pages, random: stays raw
 *   chunk 2  64 pages, repeating: compresses
 *   chunk 3  64 pages, zero
 *   chunk 4  every third page, random: not adjacent, so gathered
 *   chunk 5  nothing held
 *   chunk 6  the first and the last ten pages, repeating: two runs
 *   chunk 7  64 pages, random
 *   chunk 8  the 17 pages that are left, repeating
 */
class TestRam final {
public:
	static constexpr std::size_t prefix_pages = 20;
	static constexpr std::size_t chunk_pages = 64;
	static constexpr std::size_t chunks = 9;

	TestRam()
		: page_(host_page()), pages_((chunks - 1) * chunk_pages + 17)
	{
		/* The managed pages begin at page 20, so chunk 0 is the 44 pages
		 * after it and chunk 1 starts at page 64. */
		size_ = pages_ * page_;
		void *mapping = mmap(nullptr, size_, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANON, -1, 0);
		require(mapping != MAP_FAILED, "anonymous mapping failed");
		root_ = static_cast<std::byte *>(mapping);
		/* Only the held pages are ever written, so the pages the guest does
		 * not hold stay out of the footprint as they would in a guest. */
		fill();
		commitment_ = std::make_unique<GuestRamCommitment>(root_, size_,
			prefix_pages * page_, 0, true, page_);
		commit_all();
	}

	~TestRam()
	{
		(void)munlock(root_, size_);
		(void)munmap(root_, size_);
	}

	TestRam(const TestRam &) = delete;
	TestRam &operator=(const TestRam &) = delete;

	[[nodiscard]] GuestRamCommitment &commitment() { return *commitment_; }
	[[nodiscard]] std::size_t page() const { return page_; }
	[[nodiscard]] std::byte *root() const { return root_; }

	/* Pages of chunk `chunk` that hold content, by bit. */
	[[nodiscard]] static std::uint64_t held_bits(std::size_t chunk)
	{
		switch (chunk) {
		case 0:
			return ~std::uint64_t{0} << 20;
		case 4: {
			std::uint64_t bits = 0;
			for (std::size_t bit = 0; bit < 64; bit += 3)
				bits |= std::uint64_t{1} << bit;
			return bits;
		}
		case 5:
			return 0;
		case 6:
			return 0x3ffULL | (0x3ffULL << 54);
		case 8:
			return (std::uint64_t{1} << 17) - 1;
		default:
			return ~std::uint64_t{0};
		}
	}

	[[nodiscard]] static Fill kind_of(std::size_t chunk)
	{
		switch (chunk) {
		case 1:
		case 4:
		case 7:
			return Fill::random;
		case 3:
			return Fill::zero;
		default:
			return Fill::repeating;
		}
	}

	[[nodiscard]] std::size_t page_index(std::size_t chunk,
		std::size_t bit) const
	{
		return chunk * chunk_pages + bit;
	}

	/* Writes the content of every held page. */
	void fill(std::uint64_t salt = 0)
	{
		for (std::size_t chunk = 0; chunk < chunks; ++chunk)
			for (std::size_t bit = 0; bit < chunk_pages; ++bit)
				if ((held_bits(chunk) >> bit) & 1U)
					fill_page(root_ + page_index(chunk, bit) * page_, page_,
						page_index(chunk, bit), kind_of(chunk), salt);
	}

	/* The held pages against what `fill` wrote, as an error text or empty. */
	[[nodiscard]] std::string check(std::uint64_t salt = 0) const
	{
		std::vector<std::byte> expected(page_);
		for (std::size_t chunk = 0; chunk < chunks; ++chunk)
			for (std::size_t bit = 0; bit < chunk_pages; ++bit) {
				if (((held_bits(chunk) >> bit) & 1U) == 0)
					continue;
				fill_page(expected.data(), page_, page_index(chunk, bit),
					kind_of(chunk), salt);
				if (std::memcmp(expected.data(),
					    root_ + page_index(chunk, bit) * page_, page_) != 0)
					return "chunk " + std::to_string(chunk) + " page " +
						std::to_string(bit) + " differs";
			}
		return {};
	}

	[[nodiscard]] std::size_t held_pages() const
	{
		std::size_t count = 0;
		for (std::size_t chunk = 0; chunk < chunks; ++chunk)
			count += static_cast<std::size_t>(
				__builtin_popcountll(held_bits(chunk)));
		return count;
	}

	[[nodiscard]] const std::byte *page_at(std::size_t chunk,
		std::size_t bit) const
	{
		return root_ + page_index(chunk, bit) * page_;
	}

	/* What the commitment says is held, chunk by chunk. */
	[[nodiscard]] bool commitment_matches() const
	{
		for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
			const std::uint64_t bits = commitment_->held_word(
				prefix_pages / chunk_pages + chunk);
			if (bits != held_bits(chunk))
				return false;
		}
		return true;
	}

private:
	void commit_all()
	{
		for (std::size_t chunk = 0; chunk < chunks; ++chunk)
			for (std::size_t bit = 0; bit < chunk_pages; ++bit)
				if ((held_bits(chunk) >> bit) & 1U)
					require(commitment_->commit(
						page_index(chunk, bit) * page_, page_) == 0,
						"commit failed");
	}

	std::size_t page_;
	std::size_t pages_;
	std::size_t size_{};
	std::byte *root_{};
	std::unique_ptr<GuestRamCommitment> commitment_;
};

[[nodiscard]] std::string image_path(const char *name)
{
	auto path = uml::testing::scratch_directory() /
		(std::string{name} + "-" + std::to_string(getpid()) + ".img");
	std::error_code ignored;
	std::filesystem::remove(path, ignored);
	return path.string();
}

/* The descriptors of the process's regular files that no longer have a name.
 * A RAM image is one: it is unlinked as it is created, so these tests reach
 * its file through the descriptor the image holds. */
[[nodiscard]] std::set<int> unnamed_files()
{
	std::set<int> found;
	for (int descriptor = 3; descriptor < 1024; ++descriptor) {
		struct stat details{};
		if (fstat(descriptor, &details) == 0 && S_ISREG(details.st_mode) &&
		    details.st_nlink == 0)
			found.insert(descriptor);
	}
	return found;
}

std::map<const GuestRamImage *, int> image_files;

[[nodiscard]] std::unique_ptr<GuestRamImage> make_image(TestRam &ram,
	const char *name, bool compress = true, unsigned threads = 3)
{
	GuestRamImage::Options options;
	options.path = image_path(name);
	options.threads = threads;
	options.compress = compress;
	const std::set<int> before = unnamed_files();
	auto image = GuestRamImage::create(ram.commitment(), options);
	require(image != nullptr, "the image could not be created");
	require(!std::filesystem::exists(options.path),
		"the image shipped in production has no name once it exists");
	std::set<int> created = unnamed_files();
	for (const int descriptor : before)
		created.erase(descriptor);
	require(created.size() == 1, "the image holds exactly one unnamed file");
	image_files[image.get()] = *created.begin();
	return image;
}

/* The image's file, through the descriptor it holds. */
[[nodiscard]] int image_file(const GuestRamImage &image)
{
	const auto found = image_files.find(&image);
	require(found != image_files.end(), "not an image made by make_image");
	return found->second;
}

[[nodiscard]] format::Header read_header(const GuestRamImage &image)
{
	format::Header header{};
	require(pread(image_file(image), &header, sizeof header, 0) ==
		static_cast<ssize_t>(sizeof header), "short header");
	return header;
}

[[nodiscard]] std::vector<format::Entry> read_table(const GuestRamImage &image,
	std::size_t count)
{
	std::vector<format::Entry> table(count);
	require(pread(image_file(image), table.data(),
		count * sizeof(format::Entry), format::table_offset()) ==
		static_cast<ssize_t>(count * sizeof(format::Entry)), "short table");
	return table;
}

/* Flips one byte of the file. */
void damage(const GuestRamImage &image, std::uint64_t offset)
{
	const int fd = image_file(image);
	unsigned char byte = 0;
	require(pread(fd, &byte, 1, static_cast<off_t>(offset)) == 1,
		"cannot read the byte to damage");
	byte ^= 0x5a;
	require(pwrite(fd, &byte, 1, static_cast<off_t>(offset)) == 1,
		"cannot damage the image");
}

void hibernate_now(GuestRamImage &image, ImageStats &stats,
	bool expect_complete = true)
{
	require(image.freeze_changes(), "no change may be in flight");
	require(image.hibernate(stats) == expect_complete, "hibernate result");
}

void the_hash_sees_every_byte_and_the_length()
{
	std::vector<unsigned char> data(mib);
	std::uint64_t state = 7;
	for (auto &byte : data)
		byte = static_cast<unsigned char>(splitmix(state));
	const std::uint64_t reference = format::hash(data.data(), data.size());
	require(reference == format::hash(data.data(), data.size()),
		"the hash is not repeatable");
	require(reference != format::hash(data.data(), data.size(), 1),
		"the seed must change the hash");
	/* Every byte of a block, the first and last of the buffer, and the
	 * ragged tail all count. */
	for (std::size_t at : {std::size_t{0}, std::size_t{1}, std::size_t{63},
		     std::size_t{64}, std::size_t{4095}, std::size_t{4096},
		     mib / 2, mib - 65, mib - 1}) {
		data[at] ^= 0x01;
		require(format::hash(data.data(), data.size()) != reference,
			"a flipped bit went unseen");
		data[at] ^= 0x01;
	}
	require(format::hash(data.data(), data.size()) == reference,
		"restoring the byte must restore the hash");
	/* Swapping two blocks changes the hash: it depends on order. */
	std::vector<unsigned char> swapped = data;
	std::swap_ranges(swapped.begin(), swapped.begin() + 64,
		swapped.begin() + 64);
	require(format::hash(swapped.data(), swapped.size()) != reference,
		"the hash must depend on the order of blocks");
	/* All-zero buffers of different lengths differ. */
	std::vector<unsigned char> zero(4096);
	require(format::hash(zero.data(), 64) != format::hash(zero.data(), 128),
		"the length must change the hash");
	require(format::hash(zero.data(), 0) != format::hash(zero.data(), 1),
		"an empty buffer must differ from a byte");
	/* An unaligned, ragged length. */
	require(format::hash(data.data() + 3, 1001) !=
		format::hash(data.data() + 3, 1002), "ragged lengths differ");
}

void a_restore_returns_every_held_page_exactly_and_wired()
{
	TestRam ram;
	auto image = make_image(ram, "round-trip");
	require(image->chunk_count() == TestRam::chunks,
		"the grid must cover the managed pages");
	require(image->chunk_bytes() == 64 * ram.page(), "one chunk is 64 pages");

	const std::uint64_t before = task_footprint();
	ImageStats written;
	hibernate_now(*image, written);
	const std::uint64_t after = task_footprint();
	require(image->evicted(), "chunks must be out of the process");
	require(written.chunks_held == TestRam::chunks - 1,
		"every chunk with a held page is taken");
	require(written.chunks_kept == 0, "nothing is kept the first time");
	require(written.chunks_zero == 1, "the one zero chunk is not stored");
	require(written.evicted_bytes == ram.held_pages() * ram.page(),
		"every held page must leave the process");
	require(written.chunks_failed == 0 && written.error == 0, "no failure");
	require(written.stored_bytes < written.evicted_bytes,
		"repeating and zero chunks must shrink the image");
	require(written.written_bytes >= written.stored_bytes,
		"stored bytes were written");
	if (wiring_works) {
		require(before - after >= written.evicted_bytes / 2,
			"evicted pages must leave the footprint");
		for (std::size_t chunk = 0; chunk < TestRam::chunks; ++chunk) {
			if (chunk == 5)
				continue;
			const unsigned bit = static_cast<unsigned>(
				__builtin_ctzll(TestRam::held_bits(chunk)));
			require(wired_count(ram.page_at(chunk, bit)) == 0,
				"evicted pages must be unwired");
		}
	}
	/* Held state is untouched: the guest still holds these pages. */
	require(ram.commitment_matches(), "hibernation must not change what the "
		"guest holds");

	/* The file is the format the header says. */
	const auto header = read_header(*image);
	require(std::memcmp(header.magic, "UMLHIB01", 8) == 0, "magic");
	require(header.version == format::version && header.page_size == ram.page() &&
		header.chunk_pages == 64 && header.chunk_count == TestRam::chunks &&
		header.first_page == 0 && header.generation == 1,
		"header fields");
	require(header.header_hash == format::hash(&header,
		offsetof(format::Header, header_hash)), "header hash");
	const auto table = read_table(*image, TestRam::chunks);
	require(header.table_hash == format::hash(table.data(),
		table.size() * sizeof(format::Entry)), "table hash");
	using format::Kind;
	require(table[0].kind == Kind::lz4 && table[0].mask == TestRam::held_bits(0),
		"a repeating chunk compresses");
	require(table[1].kind == Kind::raw && table[1].stored == 64 * ram.page(),
		"an incompressible chunk stays raw");
	require(table[3].kind == Kind::zero && table[3].stored == 0,
		"a zero chunk stores nothing");
	require(table[4].kind == Kind::raw &&
		table[4].stored == 22 * ram.page() &&
		table[4].mask == TestRam::held_bits(4),
		"scattered pages are stored one after the other");
	require(table[5].kind == Kind::empty && table[5].mask == 0,
		"an empty chunk is recorded as empty");
	require(table[6].kind == Kind::lz4 && table[6].mask == TestRam::held_bits(6),
		"two runs are one chunk");

	ImageStats restored;
	require(image->restore(restored), "restore failed");
	require(restored.chunks_failed == 0 && restored.error == 0, "no failure");
	require(restored.evicted_bytes == written.evicted_bytes,
		"every evicted byte comes back");
	require(!image->evicted(), "nothing is left out");
	require(ram.check().empty(), ram.check());
	require(ram.commitment_matches(), "the held pages are as they were");
	require(!ram.commitment().changes_held(), "changes are allowed again");
	if (wiring_works)
		for (std::size_t chunk = 0; chunk < TestRam::chunks; ++chunk) {
			if (chunk == 5)
				continue;
			const unsigned bit = static_cast<unsigned>(
				__builtin_ctzll(TestRam::held_bits(chunk)));
			require(wired_count(ram.page_at(chunk, bit)) > 0,
				"restored pages must be wired again");
		}
	require(restored.wire_failures == 0, "no page failed to wire");
}

void without_compression_every_chunk_is_stored_raw()
{
	TestRam ram;
	auto image = make_image(ram, "raw", false);
	ImageStats written;
	hibernate_now(*image, written);
	const auto table = read_table(*image, TestRam::chunks);
	using format::Kind;
	for (std::size_t chunk = 0; chunk < TestRam::chunks; ++chunk)
		require(table[chunk].kind == (chunk == 3 ? Kind::zero :
			chunk == 5 ? Kind::empty : Kind::raw), "raw or zero or empty");
	ImageStats restored;
	require(image->restore(restored), "restore failed");
	require(ram.check().empty(), ram.check());
}

void a_second_image_rewrites_only_what_changed()
{
	TestRam ram;
	auto image = make_image(ram, "incremental");
	ImageStats first;
	hibernate_now(*image, first);
	ImageStats restored;
	require(image->restore(restored), "restore failed");

	/* Nothing changed since: every held chunk is kept and nothing but the
	 * table is written. */
	ImageStats second;
	hibernate_now(*image, second);
	require(second.chunks_written == 0 &&
		second.chunks_kept == TestRam::chunks - 1,
		"an unchanged image rewrites no chunk");
	require(second.written_bytes < first.written_bytes / 4,
		"an unchanged image writes only its table");
	require(second.stored_bytes == first.stored_bytes, "same image");
	require(image->restore(restored), "restore failed");
	require(ram.check().empty(), ram.check());

	/* One page of chunk 2 changes, one page of chunk 7. */
	std::memset(const_cast<std::byte *>(ram.page_at(2, 5)), 0x77, ram.page());
	std::memset(const_cast<std::byte *>(ram.page_at(7, 63)), 0x66, ram.page());
	ImageStats third;
	hibernate_now(*image, third);
	require(third.chunks_written == 2 &&
		third.chunks_kept == TestRam::chunks - 3,
		"only the two changed chunks are rewritten");
	require(image->restore(restored), "restore failed");
	for (std::size_t at = 0; at < ram.page(); ++at) {
		require(ram.page_at(2, 5)[at] == std::byte{0x77} &&
			ram.page_at(7, 63)[at] == std::byte{0x66},
			"changed pages come back as changed");
	}

	/* A chunk whose held pages changed but not their content is rewritten:
	 * the image stores pages in order, so a different set is a different
	 * chunk. Release one page of chunk 2 and commit it again elsewhere. */
	require(ram.commitment().release(ram.page_index(7, 20) * ram.page(),
		ram.page()) == 0, "release failed");
	ImageStats fourth;
	hibernate_now(*image, fourth);
	require(fourth.chunks_written == 1, "a changed set rewrites the chunk");
	require(image->restore(restored), "restore failed");
	require(restored.chunks_failed == 0, "no failure");
	/* The released page is not touched by the image and stays released. */
	require(ram.commitment().held_word(TestRam::prefix_pages / 64 + 7) ==
		(~std::uint64_t{0} & ~(std::uint64_t{1} << 20)),
		"the released page stays released");

	/* A page that turns zero changes the chunk, and a chunk of zeros is not
	 * written at all. */
	for (std::size_t bit = 0; bit < 64; ++bit)
		std::memset(const_cast<std::byte *>(ram.page_at(1, bit)), 0,
			ram.page());
	ImageStats fifth;
	hibernate_now(*image, fifth);
	require(fifth.chunks_written == 1 && fifth.chunks_zero == 2,
		"a chunk that became zero is stored as zero");
	require(fifth.written_bytes < first.written_bytes / 4,
		"a zero chunk costs no data");
	require(image->restore(restored), "restore failed");
	for (const std::size_t bit : {std::size_t{0}, std::size_t{31},
		     std::size_t{63}})
		require(std::all_of(ram.page_at(1, bit),
			ram.page_at(1, bit) + ram.page(),
			[](std::byte byte) { return byte == std::byte{0}; }),
			"zero pages come back zero, not as what the host left there");
}

void a_damaged_chunk_fails_the_restore_and_keeps_the_guest_stopped()
{
	TestRam ram;
	auto image = make_image(ram, "damaged-slot");
	ImageStats written;
	hibernate_now(*image, written);
	/* Chunk 1 is stored raw: one flipped byte in the middle of it. */
	const std::uint64_t slot = format::data_offset(TestRam::chunks) +
		1 * image->chunk_bytes();
	damage(*image, slot + 1234);
	ImageStats restored;
	require(!image->restore(restored), "a damaged chunk must fail the restore");
	require(restored.chunks_failed == 1 && restored.error == EILSEQ,
		"exactly the damaged chunk fails");
	require(ram.commitment().changes_held(),
		"a failed restore must not let the guest change its memory");
	require(image->evicted(), "the damaged chunk is still out");
}

void a_damaged_compressed_chunk_fails_the_restore()
{
	TestRam ram;
	auto image = make_image(ram, "damaged-lz4");
	ImageStats written;
	hibernate_now(*image, written);
	const std::uint64_t slot = format::data_offset(TestRam::chunks) +
		2 * image->chunk_bytes();
	damage(*image, slot + 5);
	ImageStats restored;
	require(!image->restore(restored), "a damaged chunk must fail the restore");
	require(restored.chunks_failed >= 1, "the damaged chunk fails");
}

void a_damaged_header_or_table_fails_the_restore()
{
	for (const std::uint64_t offset : {std::uint64_t{20},
		     format::table_offset() + 8}) {
		TestRam ram;
		auto image = make_image(ram, "damaged-metadata");
		ImageStats written;
		hibernate_now(*image, written);
		damage(*image, offset);
		ImageStats restored;
		require(!image->restore(restored),
			"damaged metadata must fail the restore");
		require(restored.error == EILSEQ, "metadata mismatch");
		require(ram.commitment().changes_held(), "the guest stays stopped");
		require(image->evicted(), "nothing was brought back");
	}
}

void a_short_image_fails_the_restore()
{
	TestRam ram;
	auto image = make_image(ram, "truncated");
	ImageStats written;
	hibernate_now(*image, written);
	const std::uint64_t slot = format::data_offset(TestRam::chunks) +
		7 * image->chunk_bytes();
	require(ftruncate(image_file(*image), static_cast<off_t>(slot + 4096)) == 0,
		"cannot truncate the image");
	ImageStats restored;
	require(!image->restore(restored), "a short image must fail the restore");
	require(restored.chunks_failed >= 1, "the short chunk fails");
	require(ram.commitment().changes_held(), "the guest stays stopped");
}

/* A write that cannot finish leaves its chunk in the process, and the chunks
 * that did reach the file are still restored. */
void a_failed_write_keeps_the_chunk_resident()
{
	TestRam ram;
	auto image = make_image(ram, "write-failure");
	struct rlimit original{};
	require(getrlimit(RLIMIT_FSIZE, &original) == 0, "getrlimit");
	void (*previous)(int) = std::signal(SIGXFSZ, SIG_IGN);
	/* Slots up to chunk 3 fit; chunks 4 and beyond start past the limit. */
	struct rlimit limit = original;
	limit.rlim_cur = format::data_offset(TestRam::chunks) +
		4 * image->chunk_bytes();
	require(setrlimit(RLIMIT_FSIZE, &limit) == 0, "setrlimit");
	ImageStats written;
	require(image->freeze_changes(), "freeze");
	const bool complete = image->hibernate(written);
	require(setrlimit(RLIMIT_FSIZE, &original) == 0, "restore limit");
	std::signal(SIGXFSZ, previous);
	require(!complete, "a failed write must be reported");
	/* Chunks 4, 6, 7 and 8 need data past the limit. */
	require(written.chunks_failed == 4 && written.error == EFBIG,
		"the chunks past the limit fail");
	require(image->evicted(), "the chunks that were written still leave");
	require(written.evicted_bytes ==
		(ram.held_pages() - 22 - 20 - 64 - 17) * ram.page(),
		"the failed chunks stay in the process");
	if (wiring_works) {
		require(wired_count(ram.page_at(4, 0)) > 0 &&
			wired_count(ram.page_at(7, 0)) > 0,
			"a chunk that was not stored stays wired");
		require(wired_count(ram.page_at(1, 0)) == 0,
			"a stored chunk is unwired");
	}
	ImageStats restored;
	require(image->restore(restored), "the stored chunks restore");
	require(ram.check().empty(), ram.check());
	require(!ram.commitment().changes_held(), "changes are allowed again");

	/* Space again: the chunks that failed are written the next time. */
	ImageStats again;
	hibernate_now(*image, again);
	require(again.chunks_written == 4 && again.chunks_failed == 0,
		"the chunks that failed are stored now, the rest kept");
	require(image->restore(restored), "restore failed");
	require(ram.check().empty(), ram.check());
}

void an_abort_leaves_the_unreached_chunks_resident()
{
	TestRam ram;
	auto image = make_image(ram, "abort");
	image->request_abort();
	ImageStats written;
	hibernate_now(*image, written, false);
	require(!image->evicted(), "nothing left the process");
	require(written.chunks_skipped == TestRam::chunks - 1,
		"every chunk with a held page was skipped");
	require(written.evicted_bytes == 0, "no byte was evicted");
	ImageStats restored;
	require(image->restore(restored), "an empty restore succeeds");
	require(!ram.commitment().changes_held(), "changes are allowed again");
	require(ram.check().empty(), ram.check());
	image->clear_abort();
	hibernate_now(*image, written);
	require(image->evicted(), "after the abort is cleared chunks leave");
	require(image->restore(restored), "restore failed");
	require(ram.check().empty(), ram.check());
}

void commits_wait_for_the_restore()
{
	TestRam ram;
	auto image = make_image(ram, "gate");
	ImageStats written;
	hibernate_now(*image, written);
	std::atomic<bool> committed{false};
	const std::uint64_t offset = ram.page_index(5, 3) * ram.page();
	std::thread guest{[&] {
		require(ram.commitment().commit(offset, ram.page()) == 0,
			"commit failed");
		committed.store(true);
	}};
	std::this_thread::sleep_for(std::chrono::milliseconds{150});
	require(!committed.load(), "a commit must wait while the image is out");
	ImageStats restored;
	require(image->restore(restored), "restore failed");
	guest.join();
	require(committed.load(), "the commit goes through after the restore");
	require(ram.commitment().held_word(TestRam::prefix_pages / 64 + 5) ==
		(std::uint64_t{1} << 3), "the page was committed");
}

void freeze_refuses_a_second_hibernate_and_a_missing_freeze()
{
	TestRam ram;
	auto image = make_image(ram, "freeze");
	ImageStats written;
	require(!image->hibernate(written) && written.error == EPERM,
		"hibernate needs the changes frozen");
	require(!image->evicted(), "nothing happened");
	require(image->freeze_changes(), "freeze");
	require(image->hibernate(written), "hibernate");
	ImageStats again;
	require(image->hibernate(again), "a second hibernate has nothing to do");
	require(again.chunks_held == 0 && again.evicted_bytes == 0,
		"and does nothing");
	ImageStats restored;
	require(image->restore(restored), "restore failed");
	require(ram.check().empty(), ram.check());
}

void an_image_restores_without_owning_its_old_path()
{
	TestRam ram;
	GuestRamImage::Options options;
	options.path = image_path("unlinked");
	auto image = GuestRamImage::create(ram.commitment(), options);
	require(image != nullptr, "the image could not be created");
	require(!std::filesystem::exists(options.path),
		"an image must be unlinked before it is used");
	/* Reusing the name must neither replace the RAM image's contents nor
	 * let its destructor delete another file. */
	const int replacement = open(options.path.c_str(),
		O_RDWR | O_CREAT | O_EXCL, 0600);
	require(replacement >= 0, "the old name must be free to reuse");
	const char sentinel[] = "another file";
	require(write(replacement, sentinel, sizeof sentinel) == sizeof sentinel,
		"write the replacement");
	for (int cycle = 0; cycle < 2; ++cycle) {
		ImageStats written;
		hibernate_now(*image, written);
		require(written.evicted_bytes == ram.held_pages() * ram.page(),
			"every held page must leave the process");
		ImageStats restored;
		require(image->restore(restored), "restore the unlinked image");
		require(ram.check().empty(), ram.check());
	}
	image.reset();
	require(std::filesystem::exists(options.path),
		"closing the image must not unlink the replacement");
	char actual[sizeof sentinel] {};
	require(pread(replacement, actual, sizeof actual, 0) == sizeof actual &&
		std::memcmp(actual, sentinel, sizeof actual) == 0,
		"hibernation must not write into the replacement");
	close(replacement);
	unlink(options.path.c_str());
}

/* An image has no name and no lock, so a process that ends leaves nothing.
 * The images older versions left, named for their process and locked while
 * it lived, are still removed once their lock is free. */
void the_image_file_is_exclusive_and_stale_files_are_removed()
{
	const uml::testing::ScratchSubdirectory scratch{"stale.XXXXXX"};
	const auto &directory = scratch.path();
	const auto name = [&](int pid) {
		return (directory / uml::memory::guest_ram_image_name(pid)).string();
	};
	/* Leftovers of processes that ended, and files that are not images. */
	for (const int pid : {101, 102, 103}) {
		const int fd = open(name(pid).c_str(), O_CREAT | O_WRONLY, 0600);
		require(fd >= 0, "cannot create a leftover");
		close(fd);
	}
	for (const char *other : {"notes.txt", "uml-hibernation-.img",
		     "uml-hibernation-1.img.bak", "kernarium-initrd-9.cpio"}) {
		const int fd = open((directory / other).c_str(), O_CREAT | O_WRONLY,
			0600);
		require(fd >= 0, "cannot create a bystander");
		close(fd);
	}
	/* The image of an older version's live process holds its lock. */
	const std::string live = name(static_cast<int>(getpid()));
	const int live_fd = open(live.c_str(), O_CREAT | O_RDWR, 0600);
	require(live_fd >= 0, "cannot create the live image");
	require(flock(live_fd, LOCK_EX | LOCK_NB) == 0, "cannot lock it");

	/* An image never replaces a file: it is created exclusively. */
	TestRam ram;
	GuestRamImage::Options options;
	options.path = live;
	options.threads = 2;
	errno = 0;
	require(GuestRamImage::create(ram.commitment(), options) == nullptr &&
		errno == EEXIST, "an image file is created exclusively");
	require(std::filesystem::exists(live), "and the file in the way stays");

	require(uml::memory::remove_stale_guest_ram_images(directory.string()) == 3,
		"every leftover goes, and only those");
	for (const int pid : {101, 102, 103})
		require(!std::filesystem::exists(name(pid)), "a leftover remains");
	require(std::filesystem::exists(live),
		"the image of a live process must stay");
	for (const char *other : {"notes.txt", "uml-hibernation-.img",
		     "uml-hibernation-1.img.bak", "kernarium-initrd-9.cpio"})
		require(std::filesystem::exists(directory / other),
			"a file that is not an image must stay");
	require(uml::memory::remove_stale_guest_ram_images(
		(directory / "missing").string()) == 0, "a missing directory is fine");

	/* Its process ends, and the lock with it. */
	close(live_fd);
	require(uml::memory::remove_stale_guest_ram_images(directory.string()) == 1,
		"the image of an ended process goes");
	require(!std::filesystem::exists(live), "it was removed");

	/* An image made now leaves no file in the directory at all. */
	options.path = live;
	auto image = GuestRamImage::create(ram.commitment(), options);
	require(image != nullptr, "an image in a clean directory");
	require(!std::filesystem::exists(live),
		"a live image has no directory entry for a successor to find");
	image.reset();

	GuestRamImage::Options bad;
	bad.path = (directory / "missing" / "x.img").string();
	errno = 0;
	require(GuestRamImage::create(ram.commitment(), bad) == nullptr &&
		errno == ENOENT, "an image in a missing directory is refused");
}

} // namespace

int main()
{
	const uml::testing::NamedTest tests[] = {
		{"the hash sees every byte and the length",
			the_hash_sees_every_byte_and_the_length},
		{"a restore returns every held page exactly and wired",
			a_restore_returns_every_held_page_exactly_and_wired},
		{"without compression every chunk is stored raw",
			without_compression_every_chunk_is_stored_raw},
		{"a second image rewrites only what changed",
			a_second_image_rewrites_only_what_changed},
		{"a damaged chunk fails the restore and keeps the guest stopped",
			a_damaged_chunk_fails_the_restore_and_keeps_the_guest_stopped},
		{"a damaged compressed chunk fails the restore",
			a_damaged_compressed_chunk_fails_the_restore},
		{"a damaged header or table fails the restore",
			a_damaged_header_or_table_fails_the_restore},
		{"a short image fails the restore",
			a_short_image_fails_the_restore},
		{"a failed write keeps the chunk resident",
			a_failed_write_keeps_the_chunk_resident},
		{"an abort leaves the unreached chunks resident",
			an_abort_leaves_the_unreached_chunks_resident},
		{"commits wait for the restore", commits_wait_for_the_restore},
		{"freeze refuses a second hibernate and a missing freeze",
			freeze_refuses_a_second_hibernate_and_a_missing_freeze},
		{"the image file is exclusive and stale files are removed",
			the_image_file_is_exclusive_and_stale_files_are_removed},
		{"an image restores without owning its old path",
			an_image_restores_without_owning_its_old_path},
	};
	return uml::testing::runNamedTests(tests);
}
