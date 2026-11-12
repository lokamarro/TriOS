// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Every inline TLB probe against the C++ fill path: the A64 and AArch32
 * gadget streams, the A64 and AArch32 JIT, and their exclusive, atomic and
 * (on a host with SME) SME forms. The entry Tlb::translate() fills for an
 * address is the one the probe hits; an address whose entry does not hold
 * its page misses, although every other entry does hold it.
 *
 * A probe that looked at the wrong entry would only miss, and the miss path
 * would find the entry, so the TLB is laid out to tell the outcomes apart.
 * The filled entry is pointed at a host page of its own; every other free
 * entry holds the probed page's key with a decoy host page; and the guest
 * page itself holds a third pattern, which only the miss path reaches. An
 * access shows which of the three it used.
 */
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/gadgets/a32/a32_builder.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/host_capabilities.hpp"
#include "memory/tlb.hpp"
#include "require.hpp"
#include "runtime_test_route.hpp"
#include "scratch_backing.hpp"
#include "static_mmu_test_support.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <span>
#include <sys/mman.h>
#include <vector>

namespace {

namespace arm64 = ::uml::cpu::arm64;
namespace gadgets = ::uml::cpu::arm64::gadgets;
namespace jit_test = ::uml::cpu::arm64::jit::testing;
using ::uml::cpu::EngineEvent;
using ::uml::memory::Access;
using ::uml::memory::Tlb;
using ::uml::memory::TlbEntry;
using ::uml::memory::tlb_empty_key;
using ::uml::testing::require;

constexpr std::uint64_t page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr unsigned page_shift = DARWIN_UML_GUEST_PAGE_SHIFT;
constexpr unsigned long generation = 3301;
constexpr std::uint64_t code_pc = 0x8000;
/* Where in its page each access falls. */
constexpr std::size_t offset = 0x100;

/* The probed page, below 4 GiB for the AArch32 forms. The top bit of its
 * index is set, and so is the bit above the index in the value the index is
 * taken from, so an index one bit too narrow or too wide reads another entry.
 * The index is the page number's low 14 bits (page 0x16a5b: address
 * 0x16a5b000). */
constexpr std::uint64_t target = 0x16a5bULL << page_shift;
/* The next page, in another entry. */
constexpr std::uint64_t neighbour = target + page;

/* The page number one table above `key`, in the same entry. main() checks
 * that the fill path agrees. */
[[nodiscard]] constexpr std::uint64_t alias_key(std::uint64_t key) noexcept
{
	return key + (std::uint64_t{1} << ::uml::memory::tlb_index_bits);
}

/* A page in the target's entry under another key. */
constexpr std::uint64_t alias = alias_key(target >> page_shift) << page_shift;
/* Where an SME load form stores what it loaded, in an entry of its own. */
constexpr std::uint64_t spill = target + 3 * page;
static_assert(alias + page <= (std::uint64_t{1} << 32));

constexpr std::byte entry_fill{0x11};
constexpr std::byte decoy_fill{0x22};
constexpr std::byte target_fill{0x33};
constexpr std::byte neighbour_fill{0x55};
constexpr std::byte alias_fill{0x66};
constexpr std::byte spill_fill{0x77};
constexpr std::uint64_t stored = 0x4444'4444'4444'4444ULL;

enum class Engine {
	gadgets,
	jit,
	a32_gadgets,
	a32_jit,
	sme_gadgets,
	sme_jit,
};

enum class Seen {
	entry,
	decoy,
	mapping,
	nothing,
	other,
};

[[nodiscard]] const char *seen_name(Seen seen) noexcept
{
	switch (seen) {
	case Seen::entry:
		return "the filled entry";
	case Seen::decoy:
		return "a decoy in another entry";
	case Seen::mapping:
		return "the guest page (a miss)";
	case Seen::nothing:
		return "nothing";
	case Seen::other:
		return "an unexpected value";
	}
	return "?";
}

/* One instruction form: its words before the closing SVC, with the address
 * in X2 (R2), a value to store in X5 (R5) and an addend or new value in X1.
 * `loads` forms leave the old value in X0 (R0), or for an SME form in the
 * spill page; `stores` forms change the bytes at the address. */
struct Form final {
	const char *name;
	Engine engine;
	std::vector<std::uint32_t> words;
	bool loads;
	bool stores;
	/* Bytes the loaded register holds. */
	unsigned width;
	/* X4 (R4) is a store-exclusive's status, zero on success. */
	bool exclusive_status{};
};

/* A host page aligned to the guest page. */
class HostPage final {
public:
	HostPage()
		: bytes_{static_cast<std::byte *>(mmap(nullptr, page,
			  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1,
			  0))}
	{
		require(bytes_ != MAP_FAILED);
		require((reinterpret_cast<std::uintptr_t>(bytes_) &
			(page - 1)) == 0);
	}
	~HostPage() { (void)munmap(bytes_, page); }
	HostPage(const HostPage &) = delete;
	HostPage &operator=(const HostPage &) = delete;

	[[nodiscard]] std::byte *data() const noexcept { return bytes_; }
	void fill(std::byte value) noexcept
	{
		std::memset(bytes_, std::to_integer<int>(value), page);
	}

private:
	std::byte *bytes_;
};

/* The guest pages, each its own mapping of one scratch file. */
class Guest final {
public:
	Guest() : backing_{"uml-tlb-probe-set.XXXXXX", 4 * page}
	{
		require(darwin_uml_static_vm_activate(generation) == 0);
		std::size_t index = 0;
		for (const auto address : addresses) {
			require(darwin_uml_static_vm_map(
				reinterpret_cast<void *>(address),
				backing_.descriptor(), index * page, page,
				PROT_READ | PROT_WRITE, 0, 1) == 0);
			++index;
		}
	}
	~Guest() { require(darwin_uml_static_vm_destroy(generation) == 0); }
	Guest(const Guest &) = delete;
	Guest &operator=(const Guest &) = delete;

	void reset()
	{
		for (std::size_t index = 0; index < addresses.size(); ++index) {
			std::vector<std::byte> bytes(page, fills[index]);
			backing_.put(index * page, bytes);
		}
	}

	[[nodiscard]] std::byte fill_of(std::uint64_t address) const
	{
		return fills[slot(address)];
	}

	[[nodiscard]] std::vector<std::byte> read(std::uint64_t address,
		std::size_t size) const
	{
		std::vector<std::byte> bytes(size);
		backing_.get(slot(address) * page + (address & (page - 1)),
			bytes);
		return bytes;
	}

private:
	static constexpr std::array<std::uint64_t, 4> addresses{
		target, neighbour, alias, spill};
	static constexpr std::array<std::byte, 4> fills{
		target_fill, neighbour_fill, alias_fill, spill_fill};

	[[nodiscard]] static std::size_t slot(std::uint64_t address)
	{
		const auto base = address & ~(page - 1);
		for (std::size_t index = 0; index < addresses.size(); ++index)
			if (addresses[index] == base)
				return index;
		require(false, "not a guest page of this test");
		return 0;
	}

	::uml::testing::ScratchBacking backing_;
};

[[nodiscard]] std::uint64_t key_of(std::uint64_t address) noexcept
{
	return address >> page_shift;
}

/* The entry Tlb::translate() filled for the page of `address`: the one
 * whose read key is the page's. */
[[nodiscard]] std::size_t filled_entry(const Tlb &tlb, std::uint64_t address)
{
	const auto key = key_of(address);
	std::size_t found = tlb.entries.size();
	for (std::size_t index = 0; index < tlb.entries.size(); ++index)
		if (tlb.entries[index].read_page == key) {
			require(found == tlb.entries.size(),
				"two entries hold one page");
			found = index;
		}
	require(found < tlb.entries.size(), "the fill placed no entry");
	require(tlb.entries[found].write_page == key,
		"the read and write fills chose different entries");
	return found;
}

/* The entry the C++ fill path chooses for the page of `address`. */
[[nodiscard]] std::size_t fill_entry(std::uint64_t address)
{
	auto tlb = std::make_unique<Tlb>();
	tlb->prepare(generation, darwin_uml_static_vm_changes(generation));
	require(tlb->translate(address, Access::read) != nullptr);
	require(tlb->translate(address, Access::write) != nullptr);
	return filled_entry(*tlb, address);
}

[[nodiscard]] std::uintptr_t delta(const HostPage &host,
	std::uint64_t address) noexcept
{
	return reinterpret_cast<std::uintptr_t>(host.data()) -
		static_cast<std::uintptr_t>(address & ~(page - 1));
}

/* What one run of a form did. */
struct Outcome final {
	EngineEvent event{EngineEvent::fatal};
	Seen loaded{Seen::nothing};
	Seen stored{Seen::nothing};
	std::uint64_t status{};
};

class Bench final {
public:
	Bench()
		: host_{::uml::cpu::host_capabilities()}
		, sme_{host_.sme_hardware && arm64::implements_sme(host_)}
	{}

	[[nodiscard]] bool sme() const noexcept { return sme_; }

	/* Runs `form` once against an address of the probed page or another,
	 * with the target's entry filled through Tlb::translate() and pointed
	 * at the entry page, and decoys keyed by the probed page in every free
	 * entry but the one the fill path picks for it. */
	[[nodiscard]] Outcome run(const Form &form, std::uint64_t probed)
	{
		guest_.reset();
		entry_.fill(entry_fill);
		decoy_.fill(decoy_fill);
		auto tlb = std::make_unique<Tlb>();
		tlb->prepare(generation,
			darwin_uml_static_vm_changes(generation));
		require(tlb->translate(target, Access::read) != nullptr);
		require(tlb->translate(target, Access::write) != nullptr);
		auto &entry = tlb->entries[filled_entry(*tlb, target)];
		entry.read_data_minus_address = delta(entry_, target);
		entry.write_data_minus_address = delta(entry_, target);

		const auto own_entry = fill_entry(probed);
		const auto key = key_of(probed);
		const TlbEntry decoy{key, delta(decoy_, probed), key,
			delta(decoy_, probed)};
		for (std::size_t index = 0; index < tlb->entries.size();
		     ++index) {
			auto &candidate = tlb->entries[index];
			if (index != own_entry &&
			    candidate.read_page == tlb_empty_key &&
			    candidate.write_page == tlb_empty_key)
				candidate = decoy;
		}

		const auto address = probed + offset;
		Outcome outcome;
		std::uint64_t loaded = 0;
		switch (form.engine) {
		case Engine::gadgets:
		case Engine::jit:
		case Engine::sme_gadgets:
		case Engine::sme_jit: {
			arm64::CpuState cpu{};
			cpu.pc = code_pc;
			cpu.registers[1] = 1;
			cpu.registers[2] = address;
			cpu.registers[3] = spill + offset;
			cpu.registers[5] = stored;
			cpu.registers[12] = 0;
			auto words = form.words;
			words.push_back(0xd400'0001U); // svc #0
			if (form.engine == Engine::gadgets) {
				const auto built =
					gadgets::Builder{}.build(code_pc, words);
				require(built.complete);
				outcome.event =
					gadgets::execute(built.program, cpu, *tlb);
			} else if (form.engine == Engine::jit) {
				outcome.event = jit_test::run_block_on_tlb_for_testing(
					code_pc, words, cpu, *tlb);
			} else {
				outcome.event = run_streaming(form.engine, words,
					cpu, *tlb);
			}
			loaded = cpu.registers[0];
			outcome.status = cpu.registers[4];
			break;
		}
		case Engine::a32_gadgets:
		case Engine::a32_jit: {
			arm64::CpuState cpu{};
			cpu.a32 = 1;
			cpu.a32_cp15ben = 1;
			const auto key_pc = arm64::aarch32::a32_key(
				static_cast<std::uint32_t>(code_pc));
			cpu.pc = key_pc;
			cpu.registers[2] = address;
			cpu.registers[5] = static_cast<std::uint32_t>(stored);
			auto words = form.words;
			words.push_back(0xef00'0000U); // svc #0
			const auto bytes = std::as_bytes(std::span{words});
			if (form.engine == Engine::a32_gadgets) {
				const gadgets::BuildOptions options{};
				const auto output = gadgets::a32::build(
					{.key = key_pc, .code = bytes}, {},
					std::pmr::get_default_resource(), {},
					options,
					gadgets::a32::decode_profile(options), 64);
				require(output.units != 0);
				outcome.event = gadgets::execute(
					output.result.program, cpu, *tlb);
			} else {
				outcome.event =
					jit_test::run_a32_block_on_tlb_for_testing(
						key_pc, bytes, cpu, *tlb);
			}
			loaded = static_cast<std::uint32_t>(cpu.registers[0]);
			outcome.status = static_cast<std::uint32_t>(
				cpu.registers[4]);
			break;
		}
		}

		const auto pattern = [&](std::byte fill) {
			std::uint64_t value = 0;
			for (unsigned index = 0; index < form.width; ++index)
				value = value << 8 | std::to_integer<unsigned>(fill);
			return value;
		};
		if (form.loads) {
			if (form.engine == Engine::sme_gadgets ||
			    form.engine == Engine::sme_jit) {
				const auto bytes = guest_.read(spill + offset, 1);
				loaded = pattern(bytes[0]);
			}
			outcome.loaded = loaded == pattern(entry_fill) ?
					Seen::entry :
				loaded == pattern(decoy_fill) ? Seen::decoy :
				loaded == pattern(guest_.fill_of(probed)) ?
					Seen::mapping :
					Seen::other;
		}
		const auto changed = [&](std::byte now, std::byte fill) {
			return now != fill;
		};
		const auto at_mapping = guest_.read(address, 1)[0];
		const std::array touched{
			changed(entry_.data()[offset], entry_fill),
			changed(decoy_.data()[offset], decoy_fill),
			changed(at_mapping, guest_.fill_of(probed)),
		};
		const auto count = static_cast<unsigned>(touched[0]) +
			touched[1] + touched[2];
		outcome.stored = count == 0 ? Seen::nothing :
			count > 1 ? Seen::other :
			touched[0] ? Seen::entry :
			touched[1] ? Seen::decoy : Seen::mapping;
		return outcome;
	}

private:
	/* The executor's streaming entry (GadgetExecutor::run_sme_block and
	 * JitExecutor's mode dispatch), on this test's TLB: SVCR.SM and ZA set,
	 * the SME state loaded and the native FP environment left. */
	[[nodiscard]] EngineEvent run_streaming(Engine engine,
		std::span<const std::uint32_t> words, arm64::CpuState &cpu,
		Tlb &tlb)
	{
		constexpr std::uint8_t mode = arm64::svcr_sm | arm64::svcr_za;
		const auto file = arm64::SmeRegisterFile::for_host(host_);
		require(file.data() != nullptr);
		std::vector<std::byte> task(arm64::sme_storage_size(file.svl()));
		cpu.bind_sme_registers(file);
		cpu.defer_sme_load(task.data(), mode, 0);
		cpu.access_sme_state();
		arm64::restore_native_fp_environment(cpu);
		EngineEvent event = EngineEvent::fatal;
		if (engine == Engine::sme_gadgets) {
			gadgets::BuildOptions options{};
			options.svcr = mode;
			options.host_capabilities = host_;
			const auto built = gadgets::Builder{}.build(code_pc, words,
				{}, std::pmr::get_default_resource(), {}, options);
			require(built.complete);
			cpu.access_fp_state();
			std::size_t executed = 0;
			event = gadgets::execute_streaming_preserving_native_fp_environment(
				built.program, cpu, tlb, executed,
				arm64::sme_profile(host_) == arm64::SmeProfile::sme2);
		} else {
			event = jit_test::run_block_on_tlb_for_testing(code_pc,
				words, cpu, tlb, mode);
		}
		arm64::restore_native_fp_environment(cpu);
		require(arm64::host_streaming_state_clear(host_));
		return event;
	}

	::uml::cpu::HostCapabilities host_;
	bool sme_;
	Guest guest_;
	HostPage entry_;
	HostPage decoy_;
};

[[nodiscard]] std::vector<Form> forms(bool sme)
{
	std::vector<Form> list;
	for (const auto engine : {Engine::gadgets, Engine::jit}) {
		list.push_back({"ldr x0, [x2]", engine, {0xf940'0040U}, true,
			false, 8});
		list.push_back({"str x5, [x2]", engine, {0xf900'0045U}, false,
			true, 8});
		list.push_back({"ldar x0, [x2]", engine, {0xc8df'fc40U}, true,
			false, 8});
		/* The JIT leaves STLR to its semantic path, which has no
		 * inline probe. */
		if (engine == Engine::gadgets)
			list.push_back({"stlr x5, [x2]", engine, {0xc89f'fc45U},
				false, true, 8});
		list.push_back({"ldxr x0, [x2]; stxr w4, x5, [x2]", engine,
			{0xc85f'7c40U, 0xc804'7c45U}, true, true, 8, true});
		list.push_back({"ldadd x1, x0, [x2]", engine, {0xf821'0040U},
			true, true, 8});
		/* X0 (zero) differs from memory: the compare fails and only
		 * loads, through the write entry. */
		list.push_back({"cas x0, x1, [x2]", engine, {0xc8a0'7c41U},
			true, false, 8});
	}
	for (const auto engine : {Engine::a32_gadgets, Engine::a32_jit}) {
		list.push_back({"ldr r0, [r2]", engine, {0xe592'0000U}, true,
			false, 4});
		list.push_back({"str r5, [r2]", engine, {0xe582'5000U}, false,
			true, 4});
		list.push_back({"ldrex r0, [r2]; strex r4, r5, [r2]", engine,
			{0xe192'0f9fU, 0xe182'4f95U}, true, true, 4, true});
	}
	if (sme) {
		for (const auto engine : {Engine::sme_gadgets, Engine::sme_jit}) {
			/* LDR ZA[W12, 0], [X2], then STR ZA[W12, 0], [X3] into
			 * the spill page, which shows what was loaded. */
			list.push_back({"ldr za[w12, 0], [x2]", engine,
				{0xe100'0040U, 0xe120'0060U}, true, false, 1});
			/* ZA is zero: the store writes zeros. */
			list.push_back({"str za[w12, 0], [x2]", engine,
				{0xe120'0040U}, false, true, 1});
		}
	}
	return list;
}

[[nodiscard]] const char *engine_name(Engine engine) noexcept
{
	switch (engine) {
	case Engine::gadgets:
		return "A64 gadgets";
	case Engine::jit:
		return "A64 JIT";
	case Engine::a32_gadgets:
		return "A32 gadgets";
	case Engine::a32_jit:
		return "A32 JIT";
	case Engine::sme_gadgets:
		return "SME gadgets";
	case Engine::sme_jit:
		return "SME JIT";
	}
	return "?";
}

unsigned failures = 0;

void report(const Form &form, const char *probe, const char *what, Seen seen,
	Seen expected)
{
	std::fprintf(stderr, "%s %s, %s: %s %s, expected %s\n",
		engine_name(form.engine), form.name, probe, what,
		seen_name(seen), seen_name(expected));
	++failures;
}

/* Whether a run completed its access: it reached the SVC, or a block that
 * ends at the access (a JIT release store) continued past it. */
[[nodiscard]] bool completed(EngineEvent event) noexcept
{
	return event == EngineEvent::system_call ||
		event == EngineEvent::continued;
}

/* The probe of the filled page hits the entry the fill placed. */
void the_probe_hits_the_entry_the_fill_placed(Bench &bench,
	const Form &form)
{
	const auto outcome = bench.run(form, target);
	if (!completed(outcome.event)) {
		std::fprintf(stderr, "%s %s: event %d\n",
			engine_name(form.engine), form.name,
			static_cast<int>(outcome.event));
		++failures;
		return;
	}
	if (form.loads && outcome.loaded != Seen::entry)
		report(form, "the filled page", "loaded from", outcome.loaded,
			Seen::entry);
	const auto expected_store = form.stores ? Seen::entry : Seen::nothing;
	if (outcome.stored != expected_store)
		report(form, "the filled page", "stored to", outcome.stored,
			expected_store);
	if (form.exclusive_status && outcome.status != 0)
		report(form, "the filled page",
			"store-exclusive failed, touching", outcome.stored,
			expected_store);
}

/* A page the filled entry does not cover misses, whether it lies in the
 * next entry or in the target's own entry under another key: the access
 * reaches the guest page through the miss path. */
void another_page_misses(Bench &bench, const Form &form,
	std::uint64_t probed, const char *probe)
{
	const auto outcome = bench.run(form, probed);
	if (outcome.event == EngineEvent::memory_fault &&
	    outcome.stored == Seen::nothing)
		return; /* a miss that leaves the block before the access */
	if (!completed(outcome.event)) {
		std::fprintf(stderr, "%s %s, %s: event %d\n",
			engine_name(form.engine), form.name, probe,
			static_cast<int>(outcome.event));
		++failures;
		return;
	}
	if (form.loads && outcome.loaded != Seen::mapping)
		report(form, probe, "loaded from", outcome.loaded,
			Seen::mapping);
	const auto expected_store = form.stores ? Seen::mapping : Seen::nothing;
	if (outcome.stored != expected_store)
		report(form, probe, "stored to", outcome.stored,
			expected_store);
}

} // namespace

int main()
{
	const ::uml::testing::RuntimeTestRoute runtime_route;
	Bench bench;
	/* The layout the checks assume, as the fill path places the pages. */
	const auto target_entry = fill_entry(target);
	require(fill_entry(alias) == target_entry,
		"the alias page is not in the target's entry");
	require(fill_entry(neighbour) != target_entry,
		"the next page is in the target's entry");
	require(fill_entry(spill) != target_entry &&
			fill_entry(spill) != fill_entry(neighbour),
		"the spill page shares a probed page's entry");
	if (!bench.sme())
		std::printf("arm64-tlb-probe-set: no SME hardware; the SME "
			    "forms are not run\n");
	for (const auto &form : forms(bench.sme())) {
		the_probe_hits_the_entry_the_fill_placed(bench, form);
		another_page_misses(bench, form, neighbour, "the next page");
		another_page_misses(bench, form, alias,
			"a page of the target's entry");
	}
	require(failures == 0, "an inline probe disagrees with the fill path");
	std::printf("arm64-tlb-probe-set: %u-byte pages, every probe agrees\n",
		static_cast<unsigned>(page));
}
