// SPDX-License-Identifier: GPL-2.0-or-later
#include "gadget_ceiling_ideal.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "uml_darwin_static_mmu.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;

extern "C" void uml_arm64_gadget_interrupt();

namespace {

constexpr std::uint64_t body_pc = 0x1000;
/* Any page-aligned guest address the static MMU can own. The native control
 * runs on an ordinary host allocation instead. */
constexpr std::uint64_t buffer_guest_address = 0x2000'0000;
constexpr unsigned long generation = 41;
constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;

[[noreturn]] void fail(const char *message)
{
	std::fprintf(stderr, "ideal gadget ceiling: %s\n", message);
	std::abort();
}

[[nodiscard]] std::uintptr_t gadget_address(void (*gadget)()) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

/* A branch leaving the body, such as a never-taken null check, exits through
 * an interrupt whose recorded PC identifies it. */
struct Trap final {
	std::array<std::uintptr_t, 2> stream{};
	gadgets::BlockLink link{};
};

class IdealWorkload final {
public:
	explicit IdealWorkload(std::span<const std::uint32_t> body)
		: body_{body},
		  end_pc_{body_pc + body.size() * sizeof(std::uint32_t)},
		  finish_stream_{gadget_address(uml_arm64_gadget_interrupt),
			  end_pc_},
		  finish_link_{
			.stream = finish_stream_.data(),
			.instruction_count = 0,
			.guest_pc = end_pc_,
		  }
	{
		if (body.empty() || body.size() > 64)
			fail("body must fit one translation window");
		map_buffer();
		/* Every branch target inside the body becomes a block of its own,
		 * translated from that address to the end of the body, exactly as
		 * the executor would translate it on first entry. Targets are
		 * discovered lazily through the link resolver. */
		pending_.push_back(body_pc);
		while (!pending_.empty()) {
			const auto pc = pending_.back();
			pending_.pop_back();
			if (programs_.contains(pc))
				continue;
			const auto index = static_cast<std::size_t>(
				(pc - body_pc) / sizeof(std::uint32_t));
			auto result = gadgets::Builder{}.build(pc,
				body_.subspan(index),
				[this](std::uint64_t target) {
					return resolve(target);
				});
			if (!result.complete ||
			    result.program.instruction_count() !=
				    body_.size() - index)
				fail("production Builder did not translate a complete block");
			programs_.emplace(pc, std::move(result.program));
		}
		for (auto &[pc, link] : links_) {
			link.stream = programs_.at(pc).words().data();
			/* A zero link count is the benchmark's defining
			 * idealization. It keeps every production threaded stream
			 * inside gadget code instead of taking the ordinary
			 * 4,096-instruction fairness exit to the executor. */
			link.instruction_count = 0;
		}
	}

	~IdealWorkload()
	{
		(void)darwin_uml_static_vm_destroy(generation);
		if (fd_ >= 0)
			(void)close(fd_);
	}

	IdealWorkload(const IdealWorkload &) = delete;
	IdealWorkload &operator=(const IdealWorkload &) = delete;

	void reset() noexcept
	{
		for (auto *page : host_pages_)
			std::memset(page, 0, page_size);
	}

	[[nodiscard]] UmlGadgetCeilingResult run(std::uint64_t iterations)
	{
		arm64::CpuState cpu{};
		cpu.registers[19] = iterations;
		cpu.registers[21] = buffer_guest_address;
		std::size_t executed = 0;
		const auto event = gadgets::execute(
			programs_.at(body_pc), cpu, tlb_, executed);
		if (event != ::uml::cpu::EngineEvent::interrupt)
			fail("stream did not reach its final gadget interrupt");
		if (cpu.pc != end_pc_)
			fail("stream left the body through an unexpected branch");
		if (cpu.registers[19] != 0)
			fail("stream left the loop before its final iteration");
		return {cpu.registers[20], {cpu.registers[24], cpu.registers[25]}};
	}

	/* Entries are the words that begin a gadget symbol; every other word
	 * is an operand, a link or a guest PC and prints as a number. */
	void dump_stream() const
	{
		for (const auto &[pc, program] : programs_) {
			const auto words = program.words();
			std::printf("UML_GADGET_CEILING_STREAM block=0x%llx words=%zu\n",
				static_cast<unsigned long long>(pc), words.size());
			for (std::size_t index = 0; index < words.size(); ++index) {
				const auto *address =
					reinterpret_cast<const void *>(words[index]);
				Dl_info info{};
				std::string_view name;
				if (dladdr(address, &info) != 0 &&
				    info.dli_saddr == address &&
				    info.dli_sname != nullptr) {
					name = info.dli_sname;
					if (name.starts_with("_"))
						name.remove_prefix(1);
				}
				if (name.starts_with("uml_arm64_gadget_"))
					std::printf(
						"UML_GADGET_CEILING_STREAM block=0x%llx index=%zu "
						"entry=%.*s\n",
						static_cast<unsigned long long>(pc), index,
						static_cast<int>(name.size()), name.data());
				else
					std::printf(
						"UML_GADGET_CEILING_STREAM block=0x%llx index=%zu "
						"operand=0x%llx\n",
						static_cast<unsigned long long>(pc), index,
						static_cast<unsigned long long>(words[index]));
			}
		}
	}

private:
	[[nodiscard]] const gadgets::BlockLink *resolve(std::uint64_t target)
	{
		if (target == end_pc_)
			return &finish_link_;
		if (target >= body_pc && target < end_pc_) {
			const auto [entry, inserted] = links_.try_emplace(
				target, gadgets::BlockLink{.guest_pc = target});
			if (inserted)
				pending_.push_back(target);
			return &entry->second;
		}
		auto &trap = traps_[target];
		if (trap.link.stream == nullptr) {
			trap.stream = {gadget_address(uml_arm64_gadget_interrupt),
				target};
			trap.link = {
				.stream = trap.stream.data(),
				.instruction_count = 0,
				.guest_pc = target,
			};
		}
		return &trap.link;
	}

	void map_buffer()
	{
		char path[] = "/private/tmp/uml-gadget-ceiling.XXXXXX";
		fd_ = mkstemp(path);
		if (fd_ < 0)
			fail("could not create the guest buffer's backing file");
		(void)unlink(path);
		if (ftruncate(fd_, static_cast<off_t>(
			uml_gadget_ceiling_buffer_bytes)) != 0)
			fail("could not size the guest buffer's backing file");
		if (darwin_uml_static_vm_activate(generation) != 0)
			fail("could not activate a static MMU generation");
		if (darwin_uml_static_vm_map(
			    reinterpret_cast<void *>(buffer_guest_address), fd_, 0,
			    uml_gadget_ceiling_buffer_bytes, PROT_READ | PROT_WRITE,
			    0, 1) != 0)
			fail("could not map the guest buffer");
		tlb_.prepare(generation, darwin_uml_static_vm_changes(generation));
		/* Warm both permission keys of every page so no timed iteration
		 * takes the miss path. */
		for (std::size_t offset = 0;
		     offset < uml_gadget_ceiling_buffer_bytes;
		     offset += page_size) {
			auto *read = tlb_.translate(buffer_guest_address + offset,
				::uml::memory::Access::read);
			auto *write = tlb_.translate(buffer_guest_address + offset,
				::uml::memory::Access::write);
			if (read == nullptr || write != read)
				fail("guest buffer page did not translate");
			host_pages_.push_back(read);
		}
	}

	std::span<const std::uint32_t> body_;
	std::uint64_t end_pc_;
	std::array<std::uintptr_t, 2> finish_stream_;
	gadgets::BlockLink finish_link_;
	std::map<std::uint64_t, gadgets::BlockLink> links_;
	std::map<std::uint64_t, Trap> traps_;
	std::map<std::uint64_t, gadgets::Program> programs_;
	std::vector<std::uint64_t> pending_;
	std::vector<std::byte *> host_pages_;
	int fd_{-1};
	::uml::memory::Tlb tlb_;
};

std::unique_ptr<IdealWorkload> ideal;

} // namespace

void uml_gadget_ceiling_prepare_ideal(std::span<const std::uint32_t> body)
{
	ideal = std::make_unique<IdealWorkload>(body);
}

void uml_gadget_ceiling_ideal_reset()
{
	if (!ideal)
		fail("workload was not prepared outside the timed region");
	ideal->reset();
}

UmlGadgetCeilingResult uml_gadget_ceiling_ideal_workload(
	std::uint64_t iterations)
{
	if (!ideal)
		fail("workload was not prepared outside the timed region");
	return ideal->run(iterations);
}

void uml_gadget_ceiling_ideal_dump_stream()
{
	if (!ideal)
		fail("workload was not prepared outside the timed region");
	ideal->dump_stream();
}
