// SPDX-License-Identifier: GPL-2.0-or-later
// Check that a TLB replay which sends neighbouring user PTEs as one host call
// maps each page to its own frame with its own protection.
//
// The replay gathers PTEs into runs that are all absent or that map
// consecutive offsets of one backing with one protection, and sends a run when
// the next PTE breaks it, when its page-table walk ends or when the batch is
// full. Each test lays protection blocks of 1 to 130 pages across a PMD
// boundary, so that runs break mid-PMD, at a PMD's end and at the 64-page
// batch limit, and then checks every page: its contents name the page, so a
// run carried past a frame boundary maps the wrong one, and a store to a
// read-only page must fault, so a run carried past a protection change
// grants the write.
//
// Tests:
//   blocks    private anonymous pages, protections changed in blocks
//   file      the same blocks over a shared tmpfs file mapping
//   unmap     holes punched in the blocks, which must fault afterwards
//   fork      two-page blocks of a shared file mapping that a fork's replay
//             walks whole: the parent's copy-on-write changes outnumber the
//             pending ranges, which then span the blocks, so one walk meets
//             writable pages and the read-only pages in the frames after them
//   churn     a thread changes the protection of one region while the main
//             thread checks another, so batches are rejected and walked again
// Output lines are "RESULT <test> PASS|FAIL <detail>". The exit status is the
// number of failures; a test that reports nothing counts as one.

#include <atomic>
#include <csetjmp>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

long page_size;
// Pages one last-level page table maps: a PMD's span.
long pmd_pages;

sigjmp_buf fault_jump;
volatile sig_atomic_t expecting_fault;

void on_fault(int)
{
	if (!expecting_fault)
		_exit(100);
	siglongjmp(fault_jump, 1);
}

// Whether a one-byte store to @address faults. A store that lands writes
// the byte back unchanged.
bool store_faults(volatile unsigned char *address)
{
	if (sigsetjmp(fault_jump, 1) != 0) {
		expecting_fault = 0;
		return true;
	}
	expecting_fault = 1;
	const unsigned char value = *address;
	*address = value;
	expecting_fault = 0;
	return false;
}

// Whether a one-byte load from @address faults.
bool load_faults(volatile unsigned char *address)
{
	if (sigsetjmp(fault_jump, 1) != 0) {
		expecting_fault = 0;
		return true;
	}
	expecting_fault = 1;
	(void)*address;
	expecting_fault = 0;
	return false;
}

int failures;
int reported;

void result(const char *test, bool pass, const char *detail)
{
	reported++;
	std::printf("RESULT %s %s %s\n", test, pass ? "PASS" : "FAIL", detail);
	std::fflush(stdout);
	if (!pass)
		failures++;
}

std::uint64_t page_tag(std::uint64_t salt, long index)
{
	return (salt << 32) ^ (0x9e3779b97f4a7c15ULL * (index + 1));
}

void tag_page(unsigned char *page, std::uint64_t tag)
{
	std::memcpy(page, &tag, sizeof(tag));
	std::memcpy(page + page_size - sizeof(tag), &tag, sizeof(tag));
}

bool page_has_tag(const unsigned char *page, std::uint64_t tag)
{
	std::uint64_t first;
	std::uint64_t last;
	std::memcpy(&first, page, sizeof(first));
	std::memcpy(&last, page + page_size - sizeof(last), sizeof(last));
	return first == tag && last == tag;
}

// A region that starts @before pages ahead of a PMD boundary, so its blocks
// straddle a page-table walk's end.
unsigned char *map_across_pmd(long pages, long before, int flags, int fd)
{
	const long span = (pages + 2 * pmd_pages) * page_size;
	void *reserve = mmap(nullptr, span, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (reserve == MAP_FAILED)
		return nullptr;
	const auto pmd_bytes = static_cast<std::uintptr_t>(pmd_pages * page_size);
	const auto lead = static_cast<std::uintptr_t>(before * page_size);
	const auto boundary = (reinterpret_cast<std::uintptr_t>(reserve) + lead +
		pmd_bytes - 1) & ~(pmd_bytes - 1);
	const auto start = boundary - lead;
	void *region = mmap(reinterpret_cast<void *>(start), pages * page_size,
		PROT_READ | PROT_WRITE, flags | MAP_FIXED, fd, 0);
	if (region == MAP_FAILED)
		return nullptr;
	return static_cast<unsigned char *>(region);
}

// Block lengths in pages: single pages, either side of the 64-page batch
// limit, and a block that spans two batches.
constexpr long block_lengths[] = {1, 2, 1, 3, 63, 1, 64, 2, 65, 1, 130, 5};
constexpr long block_count = sizeof(block_lengths) / sizeof(block_lengths[0]);

long blocks_span()
{
	long pages = 0;
	for (long length : block_lengths)
		pages += length;
	return pages;
}

// Make alternate blocks read-only, starting with the first block when
// @first_read_only. Records each page's expected writability.
bool protect_blocks(unsigned char *region, long pages, bool first_read_only,
		    bool *writable)
{
	long page = 0;
	bool read_only = first_read_only;
	while (page < pages) {
		for (long block = 0; block < block_count && page < pages; ++block) {
			long length = block_lengths[block];
			if (length > pages - page)
				length = pages - page;
			if (mprotect(region + page * page_size, length * page_size,
				     read_only ? PROT_READ : PROT_READ | PROT_WRITE) != 0)
				return false;
			for (long index = page; index < page + length; ++index)
				writable[index] = !read_only;
			page += length;
			read_only = !read_only;
		}
	}
	return true;
}

// Check every page's tag and that a store faults exactly where the page is
// read-only. Reports the first mismatch in @detail.
bool check_pages(const unsigned char *region, long pages, std::uint64_t salt,
		 const bool *writable, char *detail, std::size_t detail_size)
{
	for (long index = 0; index < pages; ++index) {
		auto *page = const_cast<unsigned char *>(region + index * page_size);
		if (!page_has_tag(page, page_tag(salt, index))) {
			std::snprintf(detail, detail_size,
				"page %ld of %ld holds another page's contents",
				index, pages);
			return false;
		}
		const bool faulted = store_faults(page + page_size / 2);
		if (faulted == writable[index]) {
			std::snprintf(detail, detail_size,
				"page %ld of %ld: store %s on a %s page", index,
				pages, faulted ? "faulted" : "landed",
				writable[index] ? "writable" : "read-only");
			return false;
		}
	}
	std::snprintf(detail, detail_size, "%ld pages", pages);
	return true;
}

// How many neighbouring page pairs of @region sit in consecutive frames,
// from /proc/self/pagemap; -1 when the frames cannot be read.
long consecutive_frames(const unsigned char *region, long pages)
{
	const int fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0)
		return -1;
	long pairs = 0;
	std::uint64_t previous = 0;
	for (long index = 0; index < pages; ++index) {
		std::uint64_t entry = 0;
		const auto page = reinterpret_cast<std::uintptr_t>(region) /
			page_size + index;
		if (pread(fd, &entry, sizeof(entry), page * sizeof(entry)) !=
		    sizeof(entry) || !(entry >> 63) ||
		    (entry & ((1ULL << 55) - 1)) == 0) {
			close(fd);
			return -1;
		}
		const std::uint64_t frame = entry & ((1ULL << 55) - 1);
		if (index > 0 && frame == previous + 1)
			pairs++;
		previous = frame;
	}
	close(fd);
	return pairs;
}

void report_frames(const char *name, const unsigned char *region, long pages)
{
	std::printf("INFO %s consecutive-frame pairs %ld of %ld\n", name,
		consecutive_frames(region, pages), pages - 1);
}

bool run_blocks(const char *name, unsigned char *region, long pages,
		std::uint64_t salt)
{
	bool *writable = new bool[pages];
	char detail[160];
	bool pass = true;
	// Two rounds with the blocks' protections swapped, so every block is
	// replayed once read-only and once writable.
	for (int round = 0; round < 2 && pass; ++round) {
		if (!protect_blocks(region, pages, round == 0, writable)) {
			std::snprintf(detail, sizeof(detail), "mprotect failed");
			pass = false;
			break;
		}
		pass = check_pages(region, pages, salt, writable, detail,
			sizeof(detail));
	}
	result(name, pass, detail);
	delete[] writable;
	return pass;
}

void test_blocks()
{
	const long pages = 2 * blocks_span();
	unsigned char *region = map_across_pmd(pages, blocks_span() + 17,
		MAP_PRIVATE | MAP_ANONYMOUS, -1);
	if (region == nullptr) {
		result("blocks", false, "mmap failed");
		return;
	}
	for (long index = 0; index < pages; ++index)
		tag_page(region + index * page_size, page_tag(1, index));
	report_frames("blocks", region, pages);
	run_blocks("blocks", region, pages, 1);
}

// An unlinked tmpfs file of @pages tagged pages, written before anything maps
// it, so that its page-cache frames mostly follow each other in file order.
// Returns its descriptor, or -1.
int tagged_file(long pages, std::uint64_t salt)
{
	char path[] = "/tmp/tlb-runs-XXXXXX";
	const int fd = mkstemp(path);
	if (fd < 0)
		return -1;
	unlink(path);
	unsigned char *buffer = new unsigned char[page_size];
	std::memset(buffer, 0x5a, page_size);
	bool written = true;
	for (long index = 0; index < pages && written; ++index) {
		tag_page(buffer, page_tag(salt, index));
		written = pwrite(fd, buffer, page_size, index * page_size) ==
			page_size;
	}
	delete[] buffer;
	if (!written) {
		close(fd);
		return -1;
	}
	return fd;
}

void test_file()
{
	const long pages = 2 * blocks_span();
	const int fd = tagged_file(pages, 2);
	if (fd < 0) {
		result("file", false, "file setup failed");
		return;
	}
	unsigned char *region =
		map_across_pmd(pages, blocks_span() + 41, MAP_SHARED, fd);
	close(fd);
	if (region == nullptr) {
		result("file", false, "mmap failed");
		return;
	}
	// Touch every page so the replay finds them present.
	for (long index = 0; index < pages; ++index)
		(void)*static_cast<volatile unsigned char *>(region + index * page_size);
	report_frames("file", region, pages);
	run_blocks("file", region, pages, 2);
}

void test_unmap()
{
	const long pages = 2 * blocks_span();
	unsigned char *region = map_across_pmd(pages, blocks_span() + 3,
		MAP_PRIVATE | MAP_ANONYMOUS, -1);
	if (region == nullptr) {
		result("unmap", false, "mmap failed");
		return;
	}
	for (long index = 0; index < pages; ++index)
		tag_page(region + index * page_size, page_tag(3, index));
	// Punch holes of the block lengths, every other block, then check that
	// each hole faults and that each remaining page keeps its own contents.
	bool *present = new bool[pages];
	long page = 0;
	bool hole = false;
	while (page < pages) {
		for (long block = 0; block < block_count && page < pages; ++block) {
			long length = block_lengths[block];
			if (length > pages - page)
				length = pages - page;
			if (hole)
				munmap(region + page * page_size, length * page_size);
			for (long index = page; index < page + length; ++index)
				present[index] = !hole;
			page += length;
			hole = !hole;
		}
	}
	char detail[160];
	std::snprintf(detail, sizeof(detail), "%ld pages", pages);
	bool pass = true;
	for (long index = 0; index < pages && pass; ++index) {
		auto *address = region + index * page_size;
		const bool faulted = load_faults(address);
		if (faulted == present[index]) {
			std::snprintf(detail, sizeof(detail),
				"page %ld of %ld: load %s on a %s page", index, pages,
				faulted ? "faulted" : "landed",
				present[index] ? "present" : "unmapped");
			pass = false;
		} else if (present[index] &&
			   !page_has_tag(address, page_tag(3, index))) {
			std::snprintf(detail, sizeof(detail),
				"page %ld of %ld holds another page's contents",
				index, pages);
			pass = false;
		}
	}
	result("unmap", pass, detail);
	delete[] present;
}

void test_fork()
{
	// Private pages, each its own pending range once the fork write-protects
	// it: enough to fill the range table before the shared blocks, and one
	// more page above them, so the last range grows across the blocks.
	constexpr long private_pages = 48;
	const long shared_pages = 2 * blocks_span();
	const long total = private_pages + shared_pages + 2;
	auto *reserve = static_cast<unsigned char *>(mmap(nullptr,
		total * page_size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	if (reserve == MAP_FAILED) {
		result("fork", false, "mmap failed");
		return;
	}
	for (long index = 0; index < private_pages; index += 2)
		reserve[index * page_size] = 1;
	unsigned char *top = reserve + (total - 1) * page_size;
	*top = 1;
	const int fd = tagged_file(shared_pages, 5);
	void *mapped = fd < 0 ? MAP_FAILED : mmap(reserve + private_pages * page_size,
		shared_pages * page_size, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_FIXED, fd, 0);
	if (fd >= 0)
		close(fd);
	if (mapped == MAP_FAILED) {
		result("fork", false, "shared mmap failed");
		return;
	}
	// Store to every page, so each PTE is young and dirty and the host maps
	// the writable blocks writable.
	auto *shared = static_cast<unsigned char *>(mapped);
	for (long index = 0; index < shared_pages; ++index)
		store_faults(shared + index * page_size + page_size / 2);
	report_frames("fork", shared, shared_pages);
	// Two-page blocks, writable first: a run that ignored protection would
	// carry a writable page's protection into the read-only page after it
	// wherever the two share consecutive frames, which a third of them do.
	bool *writable = new bool[shared_pages];
	char detail[160];
	bool pass = true;
	for (long page = 0; page < shared_pages && pass; page += 2) {
		const bool read_only = page % 4 == 2;
		const long length = page + 2 <= shared_pages ? 2 : 1;
		pass = mprotect(shared + page * page_size, length * page_size,
			read_only ? PROT_READ : PROT_READ | PROT_WRITE) == 0;
		for (long index = page; index < page + length; ++index)
			writable[index] = !read_only;
	}
	if (!pass)
		std::snprintf(detail, sizeof(detail), "mprotect failed");
	if (pass) {
		const pid_t child = fork();
		if (child == 0)
			_exit(0);
		int status = 0;
		if (child < 0 || waitpid(child, &status, 0) != child) {
			std::snprintf(detail, sizeof(detail), "fork failed");
			pass = false;
		}
	}
	if (pass)
		pass = check_pages(shared, shared_pages, 5, writable, detail,
			sizeof(detail));
	result("fork", pass, detail);
	delete[] writable;
}

struct Churn {
	unsigned char *region;
	long pages;
	std::atomic<bool> stop;
	std::atomic<long> rounds;
};

void *churn_protections(void *argument)
{
	auto *churn = static_cast<Churn *>(argument);
	bool read_only = true;
	while (!churn->stop.load(std::memory_order_relaxed)) {
		for (long page = 0; page < churn->pages; page += 7)
			mprotect(churn->region + page * page_size,
				7 * page_size,
				read_only ? PROT_READ : PROT_READ | PROT_WRITE);
		read_only = !read_only;
		churn->rounds.fetch_add(1, std::memory_order_relaxed);
	}
	return nullptr;
}

void test_churn()
{
	const long pages = 2 * blocks_span();
	unsigned char *region = map_across_pmd(pages, blocks_span() + 29,
		MAP_PRIVATE | MAP_ANONYMOUS, -1);
	Churn churn{};
	churn.pages = 7 * 64;
	churn.region = static_cast<unsigned char *>(mmap(nullptr,
		churn.pages * page_size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0));
	if (region == nullptr || churn.region == MAP_FAILED) {
		result("churn", false, "mmap failed");
		return;
	}
	for (long index = 0; index < pages; ++index)
		tag_page(region + index * page_size, page_tag(4, index));
	pthread_t thread;
	if (pthread_create(&thread, nullptr, churn_protections, &churn) != 0) {
		result("churn", false, "pthread_create failed");
		return;
	}
	// The rounds below run against the churn thread, so they begin once it
	// has made its first round; a loaded guest can schedule it only after
	// they are over.
	for (int wait = 0; wait < 10000 && churn.rounds.load() == 0; ++wait)
		usleep(1000);
	if (churn.rounds.load() == 0) {
		churn.stop.store(true);
		pthread_join(thread, nullptr);
		result("churn", false, "the churn thread never ran");
		return;
	}
	bool *writable = new bool[pages];
	char detail[160];
	bool pass = true;
	for (int round = 0; round < 8 && pass; ++round) {
		if (!protect_blocks(region, pages, round % 2 == 0, writable)) {
			std::snprintf(detail, sizeof(detail), "mprotect failed");
			pass = false;
			break;
		}
		pass = check_pages(region, pages, 4, writable, detail,
			sizeof(detail));
	}
	churn.stop.store(true);
	pthread_join(thread, nullptr);
	result("churn", pass, detail);
	delete[] writable;
}

} // namespace

int main(int argc, char **argv)
{
	page_size = sysconf(_SC_PAGESIZE);
	pmd_pages = page_size / 8;
	struct sigaction action = {};
	action.sa_handler = on_fault;
	sigemptyset(&action.sa_mask);
	sigaction(SIGSEGV, &action, nullptr);
	sigaction(SIGBUS, &action, nullptr);
	setvbuf(stdout, nullptr, _IOLBF, 0);
	const char *only = argc > 1 ? argv[1] : nullptr;
	// A test that returns without a RESULT line has not checked anything.
	const auto run = [&](const char *name, void (*test)()) {
		if (only != nullptr && std::strcmp(only, name) != 0)
			return;
		const int before = reported;
		test();
		if (reported == before)
			result(name, false, "reported nothing");
	};
	// The fork test runs first, while freed pages have not yet scattered the
	// frames a new file takes.
	run("fork", test_fork);
	run("blocks", test_blocks);
	run("file", test_file);
	run("unmap", test_unmap);
	run("churn", test_churn);
	std::printf("SUMMARY failures=%d\n", failures);
	return failures;
}
