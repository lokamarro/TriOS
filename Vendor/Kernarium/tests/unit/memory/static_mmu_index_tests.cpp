// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"
#include "static_mmu_test_support.hpp"
#include "allocation_fault.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"
#include "watchdog.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <iterator>
#include <memory>
#include <string_view>
#include <sys/mman.h>
#include <tuple>
#include <unistd.h>
#include <vector>

/* What a generation's page index answers, through the bridge only: the pages
 * it maps and what they map to, as a model that knows nothing of words, leaves
 * or slots predicts them. */

namespace {

using namespace std::chrono_literals;

using uml::testing::ScratchBacking;
using uml::testing::require;

constexpr std::string_view backing_name = "uml-static-mmu-index-XXXXXX";
constexpr unsigned long page = DARWIN_UML_GUEST_PAGE_SIZE;

[[nodiscard]] void *at(unsigned long address)
{
	return reinterpret_cast<void *>(address);
}

/* Each page of a backing starts with a tag naming it, so that reading
 * through a translation says which backing page the translation reached. */
[[nodiscard]] std::uint64_t tag_of(unsigned backing, unsigned page_number)
{
	return (std::uint64_t{backing} + 1) << 32 | page_number;
}

/* Files of their own, and aliases: further descriptors of the first files,
 * which name the same backing under another descriptor. */
class Backings final {
public:
	Backings(unsigned files, unsigned pages, unsigned aliases = 0)
		: pages_{pages}
	{
		for (unsigned index = 0; index < files; ++index) {
			files_.push_back(std::make_unique<ScratchBacking>(
				backing_name, std::size_t{pages} * page));
			for (unsigned number = 0; number < pages; ++number)
				files_.back()->put64(std::size_t{number} * page,
					tag_of(index, number));
			descriptors_.push_back(files_.back()->descriptor());
			file_of_.push_back(index);
		}
		for (unsigned index = 0; index < aliases; ++index) {
			const int copy = dup(files_[index]->descriptor());
			require(copy >= 0);
			duplicates_.push_back(copy);
			descriptors_.push_back(copy);
			file_of_.push_back(index);
		}
	}

	~Backings()
	{
		for (const auto copy : duplicates_)
			(void)close(copy);
	}

	Backings(const Backings &) = delete;
	Backings &operator=(const Backings &) = delete;

	[[nodiscard]] unsigned count() const noexcept
	{
		return static_cast<unsigned>(descriptors_.size());
	}
	[[nodiscard]] unsigned pages() const noexcept { return pages_; }
	[[nodiscard]] int fd(unsigned index) const { return descriptors_[index]; }
	/* Which file the backing is: aliases share their original's. */
	[[nodiscard]] unsigned file_of(unsigned index) const
	{
		return file_of_[index];
	}

private:
	unsigned pages_;
	std::vector<std::unique_ptr<ScratchBacking>> files_;
	std::vector<int> descriptors_;
	std::vector<unsigned> file_of_;
	std::vector<int> duplicates_;
};

/* What the model says a page is. */
struct Record final {
	unsigned backing;
	unsigned number;
	int protection;
	bool code;
	bool writable;

	bool operator==(const Record &) const = default;
};

class Model final {
public:
	explicit Model(unsigned long generation) : generation_{generation} {}

	std::map<unsigned long, Record> pages;

	void map(unsigned long address, unsigned long length, const Record &first)
	{
		for (unsigned long position = 0; position < length; position += page) {
			auto record = first;
			record.number += static_cast<unsigned>(position / page);
			pages[address + position] = record;
		}
	}

	void unmap(unsigned long address, unsigned long length)
	{
		const auto first = address & ~(page - 1);
		const auto end = (address + length + page - 1) & ~(page - 1);
		pages.erase(pages.lower_bound(first), pages.lower_bound(end));
	}

	void protect(unsigned long address, unsigned long length, int protection)
	{
		for (auto found = pages.lower_bound(address);
		     found != pages.end() && found->first < address + length;
		     ++found)
			found->second.protection = protection;
	}

	[[nodiscard]] unsigned long generation() const noexcept
	{
		return generation_;
	}

private:
	unsigned long generation_;
};

/* One of the guest's calls, to the bridge and to the model. */
class Driver final {
public:
	Driver(unsigned long generation, const Backings &backings)
		: model_{generation}, backings_{backings}
	{
		context_ = darwin_uml_static_vm_execution_context_create(generation);
		require(context_ != nullptr);
		require(darwin_uml_static_vm_activate(generation) == 0);
		tags_.resize(std::size_t{backings.count()} * backings.pages());
		for (unsigned b = 0; b < backings.count(); ++b)
			for (unsigned n = 0; n < backings.pages(); ++n)
				tags_[std::size_t{backings.file_of(b)} * backings.pages() +
					n] = tag_of(backings.file_of(b), n);
	}

	~Driver()
	{
		darwin_uml_static_vm_execution_context_destroy(context_);
		require(darwin_uml_static_vm_destroy(model_.generation()) == 0);
	}

	Driver(const Driver &) = delete;
	Driver &operator=(const Driver &) = delete;

	[[nodiscard]] Model &model() noexcept { return model_; }

	void map(unsigned long address, unsigned long length, const Record &first)
	{
		require(darwin_uml_static_vm_map_generation(model_.generation(),
				at(address), backings_.fd(first.backing),
				std::size_t{first.number} * page, length,
				first.protection, first.code ? 1 : 0,
				first.writable ? 1 : 0) == 0);
		if (batch_depth_ == 0)
			model_.map(address, length, first);
		else
			queued_.push_back({true, address, length, first});
		prune_handles();
	}

	void unmap(unsigned long address, unsigned long length)
	{
		require(darwin_uml_static_vm_unmap_generation(model_.generation(),
				at(address), length) == 0);
		if (batch_depth_ == 0)
			model_.unmap(address, length);
		else
			queued_.push_back({false, address, length, {}});
		prune_handles();
	}

	void protect(unsigned long address, unsigned long length, int protection)
	{
		require(darwin_uml_static_vm_protect(
				at(address), length, protection) == 0);
		model_.protect(address, length, protection);
		prune_handles();
	}

	void begin_batch()
	{
		require(darwin_uml_static_vm_batch_begin(model_.generation()) == 0);
		batch_depth_ = 1;
	}

	void end_batch()
	{
		require(darwin_uml_static_vm_batch_end() == 0);
		batch_depth_ = 0;
		for (const auto &operation : queued_) {
			if (operation.map)
				model_.map(operation.address, operation.length,
					operation.record);
			else
				model_.unmap(operation.address, operation.length);
		}
		queued_.clear();
		prune_handles();
	}

	void flush()
	{
		require(darwin_uml_static_vm_flush_generation(
				model_.generation()) == 0);
		model_.pages.clear();
		prune_handles();
	}

	/* A kernel uaccess that raced a copy-on-write names a backing; if it is
	 * not the page's, the page goes. */
	void discard_if_stale(unsigned long address, unsigned backing,
		unsigned number)
	{
		require(darwin_uml_static_vm_discard_if_stale(address,
				backings_.fd(backing), std::size_t{number} * page) == 0);
		if (const auto found = model_.pages.find(address & ~(page - 1));
		    found != model_.pages.end() &&
		    (found->second.backing != backing ||
		     found->second.number != number))
			model_.pages.erase(found);
		prune_handles();
	}

	/* A kernel store into a mapped page lands in its backing. */
	void write_tag(unsigned long address, std::uint64_t value)
	{
		require(darwin_uml_static_vm_copy_from_buffer(
				address, &value, sizeof value) == 0);
		if (const auto found = model_.pages.find(address & ~(page - 1));
		    found != model_.pages.end())
			tags_[std::size_t{backings_.file_of(found->second.backing)} *
				backings_.pages() + found->second.number] = value;
	}

	/* A backing is held by the pages that map it and by nothing else: when
	 * the last of them goes its slot, or its overflow entries, let go. */
	void check_backings_held()
	{
		std::vector<bool> mapped(backings_.count());
		for (const auto &[address, record] : model_.pages) {
			(void)address;
			mapped[backings_.file_of(record.backing)] = true;
		}
		require(uml::memory::testing::active_backing_mappings() ==
			static_cast<std::size_t>(
				std::count(mapped.begin(), mapped.end(), true)));
	}

	/* Everything the bridge says about `address` is what the model says. */
	void check_page(unsigned long address)
	{
		const auto found = model_.pages.find(address);
		for (const auto access : {DARWIN_UML_STATIC_READ,
			     DARWIN_UML_STATIC_WRITE, DARWIN_UML_STATIC_EXECUTE}) {
			int want = DARWIN_UML_STATIC_UNMAPPED;
			if (found != model_.pages.end()) {
				const auto &record = found->second;
				const bool permitted = access == DARWIN_UML_STATIC_EXECUTE ?
					record.code && (record.protection & PROT_EXEC) != 0 :
					access == DARWIN_UML_STATIC_WRITE ?
						(record.protection & PROT_WRITE) != 0 :
						(record.protection & PROT_READ) != 0;
				want = permitted ? DARWIN_UML_STATIC_TRANSLATED :
					DARWIN_UML_STATIC_PERMISSION;
			}
			void *plain = &want;
			void *entered = &want;
			require(darwin_uml_static_vm_translate(model_.generation(),
					address + 16, access, &plain) == want);
			require(darwin_uml_static_vm_execution_context_begin(
					context_, address) == 0);
			const int got = darwin_uml_static_vm_execution_context_translate(
				context_, model_.generation(), address + 16, access,
				&entered);
			require(darwin_uml_static_vm_execution_context_end(
					context_) == 0);
			require(got == want);
			if (want != DARWIN_UML_STATIC_TRANSLATED) {
				require(plain == nullptr && entered == nullptr);
				continue;
			}
			require(plain == entered);
			const auto &record = found->second;
			std::uint64_t value = 0;
			std::memcpy(&value, static_cast<char *>(plain) - 16,
				sizeof value);
			require(value == tags_[std::size_t{
				backings_.file_of(record.backing)} * backings_.pages() +
				record.number]);
		}

		void *handle = nullptr;
		darwin_uml_static_code_page_stamp stamp{};
		const int code = darwin_uml_static_vm_code_page(
			model_.generation(), address, &handle, &stamp);
		if (found == model_.pages.end()) {
			require(code == DARWIN_UML_STATIC_UNMAPPED);
			return;
		}
		const auto &record = found->second;
		if (!(record.code && (record.protection & PROT_EXEC) != 0)) {
			require(code == DARWIN_UML_STATIC_PERMISSION);
			return;
		}
		require(code == DARWIN_UML_STATIC_TRANSLATED && handle != nullptr);
		/* Aliases of one backing page share one record. */
		const auto [place, inserted] = handles_.emplace(
			std::pair{backings_.file_of(record.backing), record.number},
			handle);
		require(inserted || place->second == handle);
	}

private:
	/* A code-page record lives as long as its backing, which is as long as
	 * some page maps it; a backing that went away and came back has new
	 * ones. */
	void prune_handles()
	{
		for (auto found = handles_.begin(); found != handles_.end();) {
			const bool mapped = std::any_of(model_.pages.begin(),
				model_.pages.end(), [&](const auto &entry) {
					return backings_.file_of(entry.second.backing) ==
						found->first.first;
				});
			found = mapped ? std::next(found) : handles_.erase(found);
		}
	}

	struct Queued final {
		bool map;
		unsigned long address;
		unsigned long length;
		Record record;
	};

	Model model_;
	const Backings &backings_;
	void *context_{};
	unsigned batch_depth_{};
	std::vector<Queued> queued_;
	std::vector<std::uint64_t> tags_;
	std::map<std::pair<unsigned, unsigned>, void *> handles_;
};

class Random final {
public:
	explicit Random(std::uint64_t seed) : state_{seed} {}

	[[nodiscard]] std::uint64_t next()
	{
		state_ ^= state_ << 13;
		state_ ^= state_ >> 7;
		state_ ^= state_ << 17;
		return state_;
	}
	[[nodiscard]] unsigned below(unsigned limit)
	{
		return static_cast<unsigned>(next() % limit);
	}

private:
	std::uint64_t state_;
};

/* Random mapping changes agree with the model, with this many backings. */
void run_model(unsigned files, unsigned aliases, unsigned backing_pages,
	unsigned long generation, std::uint64_t seed, unsigned steps)
{
	using uml::memory::testing::active_backing_mappings;
	require(active_backing_mappings() == 0);
	const Backings backings{files, backing_pages, aliases};
	const auto backing_count = backings.count();
	{
		Driver driver{generation, backings};
		Random random{seed};
		/* Two windows of 160 pages far enough apart to live under different
		 * nodes of the index at every level, and one at the very top of the
		 * guest address space. */
		constexpr unsigned window_pages = 160;
		const std::array<unsigned long, 3> windows{0x40'0000UL,
			0x7f'f000'0000UL,
			DARWIN_UML_GUEST_ADDRESS_LIMIT - window_pages * page};
		const auto pick_page = [&] {
			return windows[random.below(3)] +
				random.below(window_pages) * page;
		};
		const int protections[] = {PROT_READ, PROT_READ | PROT_WRITE,
			PROT_READ | PROT_EXEC, PROT_READ | PROT_WRITE | PROT_EXEC,
			PROT_EXEC, PROT_NONE};
		const auto map_somewhere = [&] {
			const auto b = random.below(backing_count);
			const auto length = 1 + random.below(
				std::min(8U, backing_pages));
			const auto first = random.below(backing_pages - length + 1);
			auto address = pick_page();
			/* Stay inside the window the address is in. */
			for (const auto window : windows)
				if (address >= window &&
				    address < window + window_pages * page) {
					address = std::min(address, window +
						(window_pages - length) * page);
					break;
				}
			driver.map(address, length * page,
				{b, first, protections[random.below(6)],
					random.below(2) != 0, random.below(2) != 0});
		};
		const auto unmap_somewhere = [&] {
			auto address = pick_page();
			auto length = (1 + random.below(24)) * page;
			/* Sometimes not on a page boundary at either end. */
			if (random.below(4) == 0) {
				address += random.below(page);
				length += random.below(page);
			}
			for (const auto window : windows)
				if (address >= window &&
				    address < window + window_pages * page) {
					length = std::min<unsigned long>(length,
						window + window_pages * page - address);
					break;
				}
			driver.unmap(address, length);
		};

		for (unsigned step = 0; step < steps; ++step) {
			switch (random.below(12)) {
			case 0:
			case 1:
			case 2:
			case 3:
				map_somewhere();
				break;
			case 4:
			case 5:
				unmap_somewhere();
				break;
			case 6: {
				auto address = pick_page();
				const auto length = (1 + random.below(12)) * page;
				for (const auto window : windows)
					if (address >= window &&
					    address < window + window_pages * page) {
						address = std::min(address, window +
							window_pages * page - length);
						break;
					}
				driver.protect(address, length,
					protections[random.below(6)]);
				break;
			}
			case 7:
			case 8: {
				/* A batch, as a page-table walker publishes them. The
				 * operations of one batch may overlap. */
				driver.begin_batch();
				const auto operations = 1 + random.below(6);
				for (unsigned index = 0; index < operations; ++index) {
					if (random.below(3) == 0)
						unmap_somewhere();
					else
						map_somewhere();
				}
				driver.end_batch();
				break;
			}
			case 9: {
				const auto address = pick_page();
				const auto &pages = driver.model().pages;
				const auto found = pages.find(address);
				const bool right = found != pages.end() &&
					random.below(2) == 0;
				driver.discard_if_stale(address,
					right ? found->second.backing :
						random.below(backing_count),
					right ? found->second.number :
						random.below(backing_pages));
				break;
			}
			case 10: {
				const auto address = pick_page();
				driver.write_tag(address, (std::uint64_t{step} << 8) | 0x5a);
				break;
			}
			default:
				if (random.below(40) == 0)
					driver.flush();
				break;
			}
			driver.check_backings_held();
			for (unsigned check = 0; check < 24; ++check)
				driver.check_page(pick_page());
			if (step % 25 == 0)
				for (const auto window : windows)
					for (unsigned index = 0; index < window_pages; ++index)
						driver.check_page(window + index * page);
		}
		/* Every page the model holds, and every neighbour. */
		for (const auto window : windows)
			for (unsigned index = 0; index < window_pages; ++index)
				driver.check_page(window + index * page);

		/* Once nothing is mapped, nothing is kept alive: every slot and
		 * every overflow entry let go of its backing, whether the pages
		 * went one range at a time or all at once. */
		for (const auto window : windows)
			driver.unmap(window, window_pages * page);
		driver.check_backings_held();
		require(driver.model().pages.empty());
		driver.flush();
		for (const auto window : windows)
			for (unsigned index = 0; index < window_pages; ++index)
				driver.check_page(window + index * page);
		require(active_backing_mappings() == 0);
	}
	require(active_backing_mappings() == 0);
}

void random_changes_agree_with_a_model_over_a_few_backings()
{
	/* One of them under two descriptors. */
	run_model(3, 1, 16, 701, 0x9e3779b97f4a7c15ULL, 1500);
}

void random_changes_agree_with_a_model_over_more_backings_than_slots()
{
	/* 80 files and 8 aliases of 3 pages: more than the 63 backings a word
	 * can name, so most pages of a busy generation live in the overflow
	 * table, and slots come and go as backings lose their last page. */
	run_model(80, 8, 3, 702, 0x2545f4914f6cdd1dULL, 1500);
}

void a_generation_may_map_every_backing_it_is_given()
{
	constexpr unsigned long generation = 703;
	constexpr unsigned count = 100;
	const Backings backings{count, 2};
	Driver driver{generation, backings};
	/* One page of each, then each remapped to a neighbour's place. */
	for (unsigned index = 0; index < count; ++index)
		driver.map(0x10'0000UL + index * page, page,
			{index, index % 2, PROT_READ | PROT_WRITE, false, true});
	for (unsigned index = 0; index < count; ++index)
		driver.check_page(0x10'0000UL + index * page);
	driver.begin_batch();
	for (unsigned index = 0; index < count; ++index)
		driver.map(0x10'0000UL + index * page, page,
			{(index + 17) % count, (index + 1) % 2,
				PROT_READ | PROT_EXEC, true, false});
	driver.end_batch();
	for (unsigned index = 0; index < count; ++index)
		driver.check_page(0x10'0000UL + index * page);
	/* A stale uaccess drops a page whichever table holds it, and a right
	 * one leaves it. */
	for (unsigned index = 0; index < count; ++index) {
		const auto &record = driver.model().pages.at(
			0x10'0000UL + index * page);
		if (index % 3 == 0)
			driver.discard_if_stale(0x10'0000UL + index * page,
				record.backing, record.number);
		else if (index % 3 == 1)
			driver.discard_if_stale(0x10'0000UL + index * page,
				(record.backing + 1) % count, record.number);
	}
	for (unsigned index = 0; index < count; ++index)
		driver.check_page(0x10'0000UL + index * page);
	driver.unmap(0x10'0000UL, count * page);
	for (unsigned index = 0; index < count; ++index)
		driver.check_page(0x10'0000UL + index * page);
	require(driver.model().pages.empty());
}

/* A change that fails while it reserves has changed nothing: not a page it
 * maps, not one it removes, not the sequences a vCPU compares. */
void a_change_that_fails_in_its_reserve_step_changes_nothing()
{
	using uml::memory::testing::AllocationSite;
	using uml::memory::testing::clear_allocation_failure;
	using uml::memory::testing::fail_allocation_at;
	constexpr unsigned long generation = 704;
	constexpr unsigned count = 70;
	const Backings backings{count, 8};
	constexpr unsigned long a = 0x20'0000;
	constexpr unsigned long b = 0x7f'f000'0000UL;
	constexpr unsigned long c = 0x30'0000;

	for (const auto site : {AllocationSite::reserve,
		     AllocationSite::history, AllocationSite::batch_commit}) {
		for (const bool overflow : {false, true}) {
			Driver driver{generation, backings};
			/* In the overflow run the first 63 backings fill the slots, and
			 * the batch below maps the rest. */
			if (overflow)
				for (unsigned index = 0; index < 63; ++index)
					driver.map(0x400'0000UL + index * page, page,
						{index, 0, PROT_READ, false, false});
			driver.map(a, 6 * page, {64, 0, PROT_READ | PROT_WRITE, false, true});
			driver.map(b, 2 * page, {65, 2, PROT_READ | PROT_EXEC, true, false});
			const auto before = darwin_uml_static_vm_sequences(generation);
			const auto sweep = [&] {
				for (unsigned index = 0; index < 8; ++index) {
					driver.check_page(a + index * page);
					driver.check_page(b + index * page);
					driver.check_page(c + index * page);
				}
			};
			sweep();

			/* The same change, failing at its first, second, ... checkpoint
			 * until it has none left. */
			for (std::size_t occurrence = 1;; ++occurrence) {
				fail_allocation_at(site, occurrence);
				const bool failed = [&] {
					if (darwin_uml_static_vm_batch_begin(generation) != 0)
						return true;
					const auto map = [&](unsigned long address,
							      unsigned long pages_in,
							      unsigned backing, unsigned number) {
						return darwin_uml_static_vm_map_generation(
							generation, at(address), backings.fd(backing),
							std::size_t{number} * page, pages_in * page,
							PROT_READ | PROT_WRITE, 0, 1);
					};
					/* Replaces two pages of a mapping (its offsets move),
					 * removes one and adds pages far from the others. */
					if (map(a + page, 2, 66, 1) != 0 ||
					    darwin_uml_static_vm_unmap_generation(generation,
						    at(a + 4 * page), page) != 0 ||
					    map(c, 3, 67, 2) != 0 ||
					    map(b + 2 * page, 1, 68, 0) != 0)
						return true;
					return darwin_uml_static_vm_batch_end() != 0;
				}();
				clear_allocation_failure();
				if (!failed) {
					/* Out of checkpoints: the change went through whole. */
					driver.model().map(a + page, 2 * page,
						{66, 1, PROT_READ | PROT_WRITE, false, true});
					driver.model().unmap(a + 4 * page, page);
					driver.model().map(c, 3 * page,
						{67, 2, PROT_READ | PROT_WRITE, false, true});
					driver.model().map(b + 2 * page, page,
						{68, 0, PROT_READ | PROT_WRITE, false, true});
					sweep();
					break;
				}
				require(occurrence < 1000);
				sweep();
				const auto after = darwin_uml_static_vm_sequences(generation);
				require(after.mapping_changes == before.mapping_changes &&
					after.code_changes == before.code_changes);
			}
		}
	}
}

/* A range the width of the address space names three pages. */
void a_sparse_unmap_of_the_whole_address_space_is_quick()
{
	constexpr unsigned long generation = 705;
	constexpr unsigned long first = DARWIN_UML_NATIVE_ADDRESS_END;
	constexpr unsigned long length =
		DARWIN_UML_GUEST_ADDRESS_LIMIT - first;
	const Backings backings{2, 4};
	Driver driver{generation, backings};
	driver.map(first, page, {0, 0, PROT_READ, false, false});
	const unsigned long third = (length / 3) & ~(page - 1);
	driver.map(first + third, 2 * page,
		{1, 1, PROT_READ | PROT_EXEC, true, false});
	driver.map(first + length - page, page,
		{0, 3, PROT_READ | PROT_WRITE, false, true});
	driver.map(0x10'0000, page, {1, 0, PROT_READ, false, false});
	for (const auto [address, pages] : std::vector<std::pair<unsigned long,
		     unsigned long>>{{first, 1}, {first + third, 2},
		     {first + length - page, 1}, {0x10'0000, 1}})
		for (unsigned long index = 0; index < pages; ++index)
			driver.check_page(address + index * page);

	{
		uml::testing::Watchdog watchdog{
			"unmapping the whole address space", 20s};
		driver.begin_batch();
		driver.unmap(first, length);
		driver.end_batch();
		driver.unmap(first, length);
		driver.begin_batch();
		driver.map(first + third, page,
			{0, 2, PROT_READ, false, false});
		driver.unmap(first, length);
		driver.end_batch();
	}
	driver.check_page(first);
	driver.check_page(first + third);
	driver.check_page(first + length - page);
	driver.check_page(0x10'0000);
	require(driver.model().pages.size() == 1);
}

} // namespace

int main()
{
	const uml::testing::NamedTest tests[] = {
		{"random changes agree with a model over a few backings",
			random_changes_agree_with_a_model_over_a_few_backings},
		{"random changes agree with a model over more backings than slots",
			random_changes_agree_with_a_model_over_more_backings_than_slots},
		{"a generation may map every backing it is given",
			a_generation_may_map_every_backing_it_is_given},
		{"a change that fails in its reserve step changes nothing",
			a_change_that_fails_in_its_reserve_step_changes_nothing},
		{"a sparse unmap of the whole address space is quick",
			a_sparse_unmap_of_the_whole_address_space_is_quick},
	};
	return uml::testing::runNamedTests(tests);
}
