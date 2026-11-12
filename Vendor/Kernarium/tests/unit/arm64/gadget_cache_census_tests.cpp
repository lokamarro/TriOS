// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/gadget_cache_census.hpp"

#include "allocation_failure.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace census = uml::cpu::gadget_cache::census;

namespace {

using uml::testing::require;

constexpr const char *census_path = "DARWIN_UML_GADGET_CACHE_CENSUS";

[[nodiscard]] std::uint64_t read_fpcr() noexcept
{
	std::uint64_t value;
	__asm__ volatile("mrs %0, fpcr" : "=r"(value) : : "memory");
	return value;
}

[[nodiscard]] std::uint64_t read_fpsr() noexcept
{
	std::uint64_t value;
	__asm__ volatile("mrs %0, fpsr" : "=r"(value) : : "memory");
	return value;
}

void write_fpcr(std::uint64_t value) noexcept
{
	__asm__ volatile("msr fpcr, %0" : : "r"(value) : "memory");
}

void write_fpsr(std::uint64_t value) noexcept
{
	__asm__ volatile("msr fpsr, %0" : : "r"(value) : "memory");
}

/* The census file of one case, named for as long as the case runs. */
class CensusFile final {
public:
	CensusFile() : file_{directory_.path() / "census.txt"}
	{
		setenv(census_path, file_.c_str(), 1);
	}
	~CensusFile() { unsetenv(census_path); }
	CensusFile(const CensusFile &) = delete;
	CensusFile &operator=(const CensusFile &) = delete;

	[[nodiscard]] std::string text() const
	{
		std::ifstream in{file_};
		return {std::istreambuf_iterator<char>{in}, {}};
	}

private:
	uml::testing::ScratchSubdirectory directory_{"census-XXXXXX"};
	std::filesystem::path file_;
};

[[nodiscard]] std::string history_fields(const census::History &history)
{
	char *text = nullptr;
	std::size_t size = 0;
	std::FILE *out = open_memstream(&text, &size);
	require(out != nullptr, "no memory stream for the history");
	history.write(out);
	std::fclose(out);
	std::string fields{text, size};
	std::free(text);
	return fields + " ";
}

[[nodiscard]] bool has_field(const std::string &fields, const char *field)
{
	return fields.find(std::string{" "} + field + " ") != std::string::npos;
}

/* A cache records its departures and stores from operations that must not
 * fail, some of them noexcept. One the census has no memory to remember is
 * counted, and the rest are matched as before. */
void a_record_without_memory_is_counted()
{
	census::History history;
	history.depart(1, 0x1000, 0xabc, 64, census::Departure::retired);
	uml::testing::fail_next_allocation();
	history.depart(1, 0x2000, 0xdef, 64, census::Departure::retired);
	require(!uml::testing::cancel_pending_allocation_failure(),
		"a departure allocated nothing");
	uml::testing::fail_next_allocation();
	history.arrive(0, 1, 0x3000, 0x123, false);
	require(!uml::testing::cancel_pending_allocation_failure(),
		"a store allocated nothing");
	history.arrive(0, 1, 0x1000, 0xabc, false);
	const auto fields = history_fields(history);
	require(has_field(fields, "unrecorded=2") &&
			has_field(fields, "retirements=2") &&
			has_field(fields, "stores=2") &&
			has_field(fields, "again_retired=1"),
		fields);
}

/* A snapshot is written from a lookup or a teardown, which must not fail
 * for it: one the census has no memory to finish is left out whole, and the
 * next is written. */
void a_snapshot_without_memory_is_left_out_whole()
{
	const CensusFile file;
	census::append([](std::FILE *out) {
		std::fputs("E 0x1 partial\n", out);
		uml::testing::fail_next_allocation();
		census::RecordNames names;
		names[0x1000] = {'l', 0x2000};
	});
	census::append([](std::FILE *out) { std::fputs("E 0x2 whole\n", out); });
	const auto text = file.text();
	require(text == "E 0x2 whole\n", text);
}

/* The arm64 engine leaves the guest's FPCR and FPSR in the host registers
 * between the blocks of a run, where a lookup may write a snapshot. The
 * writer computes in the default environment, and the guest's comes back as
 * it was, with no flag of the writer's own. */
void a_snapshot_leaves_the_guest_floating_point_state()
{
	const CensusFile file;
	/* Towards zero and flushing to zero, with only QC up. */
	constexpr std::uint64_t guest_fpcr =
		std::uint64_t{3} << 22 | std::uint64_t{1} << 24;
	constexpr std::uint64_t guest_fpsr = std::uint64_t{1} << 27;
	std::uint64_t writer_fpcr = ~std::uint64_t{0};
	write_fpcr(guest_fpcr);
	write_fpsr(guest_fpsr);
	census::append([&writer_fpcr](std::FILE *out) {
		writer_fpcr = read_fpcr();
		volatile double third = 1.0;
		third = third / 3.0;
		std::fprintf(out, "E 0x1 seconds=%.3f\n", third);
	});
	const auto fpcr = read_fpcr();
	const auto fpsr = read_fpsr();
	write_fpcr(0);
	write_fpsr(0);
	require(writer_fpcr == 0, "the writer ran in the guest's FPCR");
	require(fpcr == guest_fpcr, "the guest's FPCR did not come back");
	require(fpsr == guest_fpsr, "the writer left a flag in the guest's FPSR");
	require(file.text() == "E 0x1 seconds=0.333\n", file.text());
}

/* Every 4096th lookup asks whether a periodic snapshot is due, so the
 * schedule reads the clock without raising a floating-point flag. */
void asking_for_a_snapshot_raises_no_flag()
{
	const CensusFile file;
	/* Read once by the census, before anything else asks. */
	setenv("DARWIN_UML_GADGET_CACHE_CENSUS_SECONDS", "3600", 1);
	census::Schedule schedule;
	write_fpsr(0);
	bool due = false;
	for (int ask = 0; ask < 4; ++ask)
		due = schedule.due() != 0 || due;
	const auto fpsr = read_fpsr();
	unsetenv("DARWIN_UML_GADGET_CACHE_CENSUS_SECONDS");
	require(!due, "a snapshot was due within the hour");
	require(fpsr == 0, "asking whether a snapshot is due raised a flag");
}

} // namespace

int main()
{
	unsetenv(census_path);
	unsetenv("DARWIN_UML_GADGET_CACHE_CENSUS_TOTALS");
	a_record_without_memory_is_counted();
	a_snapshot_without_memory_is_left_out_whole();
	a_snapshot_leaves_the_guest_floating_point_state();
	asking_for_a_snapshot_raises_no_flag();
	return 0;
}
