// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"
#include "architecture_difference.hpp"
#include "scratch_backing.hpp"

#include "cpu/engine.hpp"
#include "platform/apple/host_features.hpp"
#include "cpu/engine_factory.hpp"
#include "memory/static_guest_memory.hpp"

#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"
#include "static_mmu_test_support.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace uml::emulator::tests {
namespace {

using ::uml::memory::StaticGuestMemory;

constexpr std::uint64_t code_base = engine_test_code_base;
constexpr std::size_t execution_limit = 64;
constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

/* The production translated engines, in the order every case runs them. */
enum class Engine {
  gadgets,
  jit,
};

[[nodiscard]] bool engine_available(Engine engine) noexcept {
  switch (engine) {
  case Engine::gadgets:
    return ::uml::cpu::cpu_engine_available(::uml::cpu::EngineKind::gadgets);
  case Engine::jit:
    return ::uml::cpu::cpu_engine_available(::uml::cpu::EngineKind::jit);
  }
  return false;
}

struct Outcome final {
  ExpectedEvent event{ExpectedEvent::fatal};
  arm64::CpuState cpu;
  std::size_t instructions{};
  arm64::MemoryFault fault{};
  std::uint32_t syndrome{};
};

[[noreturn]] void
fail(std::string_view test_name, std::string_view engine,
     std::uint32_t instruction, std::string_view message,
     const std::source_location &location = std::source_location::current()) {
  std::fprintf(stderr,
               "%.*s: %.*s failed for instruction %#010x: %.*s at %s:%u\n",
               static_cast<int>(test_name.size()), test_name.data(),
               static_cast<int>(engine.size()), engine.data(), instruction,
               static_cast<int>(message.size()), message.data(),
               location.file_name(), location.line());
  std::abort();
}

[[nodiscard]] constexpr std::string_view engine_name(Engine engine) noexcept {
  switch (engine) {
  case Engine::gadgets:
    return "gadgets";
  case Engine::jit:
    return "jit";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view
event_name(ExpectedEvent event) noexcept {
  switch (event) {
  case ExpectedEvent::system_call:
    return "system-call";
  case ExpectedEvent::memory_fault:
    return "memory-fault";
  case ExpectedEvent::illegal_instruction:
    return "illegal-instruction";
  case ExpectedEvent::breakpoint:
    return "breakpoint";
  case ExpectedEvent::floating_point_trap:
    return "floating-point-trap";
  case ExpectedEvent::pc_alignment:
    return "pc-alignment";
  case ExpectedEvent::instruction_budget:
    return "instruction-budget";
  case ExpectedEvent::fatal:
    return "fatal";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::size_t
default_instruction_count(const InstructionCase &test_case) noexcept {
  if (test_case.event == ExpectedEvent::system_call) {
    return (test_case.instruction & 0xffe0'001fU) == 0xd400'0001U ? 1U : 2U;
  }
  if (test_case.event == ExpectedEvent::breakpoint)
    return 1U;
  return 0U;
}

[[nodiscard]] constexpr std::optional<std::size_t>
instruction_count_expectation(const InstructionCase &test_case) noexcept {
  if (test_case.expected_instructions.has_value())
    return test_case.expected_instructions;
  if (test_case.event == ExpectedEvent::instruction_budget)
    return std::nullopt;
  return default_instruction_count(test_case);
}

[[nodiscard]] constexpr ExpectedEvent
event_for(::uml::cpu::ExitReason reason) noexcept {
	switch (reason) {
	case ::uml::cpu::ExitReason::system_call:
		return ExpectedEvent::system_call;
	case ::uml::cpu::ExitReason::memory_fault:
		return ExpectedEvent::memory_fault;
	case ::uml::cpu::ExitReason::illegal_instruction:
		return ExpectedEvent::illegal_instruction;
	case ::uml::cpu::ExitReason::breakpoint:
		return ExpectedEvent::breakpoint;
	case ::uml::cpu::ExitReason::floating_point_trap:
		return ExpectedEvent::floating_point_trap;
	case ::uml::cpu::ExitReason::pc_alignment:
		return ExpectedEvent::pc_alignment;
	case ::uml::cpu::ExitReason::instruction_budget:
	case ::uml::cpu::ExitReason::interrupt:
		return ExpectedEvent::instruction_budget;
	case ::uml::cpu::ExitReason::single_step:
	case ::uml::cpu::ExitReason::sme_access:
	case ::uml::cpu::ExitReason::fatal:
		return ExpectedEvent::fatal;
	}
	return ExpectedEvent::fatal;
}

using ::uml::testing::ScratchBacking;

constexpr std::string_view backing_name = "uml-engine-test.XXXXXX";

class AddressSpace final {
public:
  explicit AddressSpace(unsigned long generation) : generation_{generation} {
    if (darwin_uml_static_vm_activate(generation_) != 0)
      fail("fixture", "mapping", 0, "cannot activate address space");
  }

  ~AddressSpace() {
    if (darwin_uml_static_vm_destroy(generation_) != 0)
      fail("fixture", "mapping", 0, "cannot destroy address space");
  }

  AddressSpace(const AddressSpace &) = delete;
  AddressSpace &operator=(const AddressSpace &) = delete;

private:
  unsigned long generation_;
};

class Mapping final {
public:
  Mapping(unsigned long generation, std::uint64_t address,
          const ScratchBacking &backing, std::size_t size, int protection,
          bool guest_code, bool guest_writable)
      : generation_{generation}, address_{address}, size_{size} {
    if (darwin_uml_static_vm_map_generation(
            generation_, reinterpret_cast<void *>(address_),
            backing.descriptor(), 0, size_, protection, guest_code ? 1 : 0,
            guest_writable ? 1 : 0) != 0)
      fail("fixture", "mapping", 0, "cannot map test region");
  }

  ~Mapping() {
    if (darwin_uml_static_vm_unmap_generation(
            generation_, reinterpret_cast<void *>(address_), size_) != 0)
      fail("fixture", "mapping", 0, "cannot unmap test region");
  }

  Mapping(const Mapping &) = delete;
  Mapping &operator=(const Mapping &) = delete;

private:
  unsigned long generation_;
  std::uint64_t address_;
  std::size_t size_;
};

[[nodiscard]] int region_protection(const MemoryRegion &region) noexcept {
  return (region.readable ? PROT_READ : 0) |
         (region.writable ? PROT_WRITE : 0) |
         (region.executable ? PROT_EXEC : 0);
}

class CaseRegion final {
public:
  CaseRegion(unsigned long generation, const MemoryRegion &region)
      : region_{region}, backing_{backing_name, region.initial.size()},
        mapping_{generation,
                 region.address,
                 backing_,
                 region.initial.size(),
                 region_protection(region),
                 region.guest_code,
                 region.guest_writable},
        actual_(region.initial.size()) {
    restore();
  }

  CaseRegion(const CaseRegion &) = delete;
  CaseRegion &operator=(const CaseRegion &) = delete;

  void restore() {
    backing_.put(0, region_.initial);
  }

  void require_expected(std::string_view test_name, std::string_view engine,
                        std::uint32_t instruction) {
    backing_.get(0, actual_);
    const auto expected =
        region_.expected.empty() ? region_.initial : region_.expected;
    for (std::size_t offset = 0; offset < actual_.size(); ++offset) {
      if (actual_[offset] == expected[offset])
        continue;
      std::fprintf(stderr,
                   "memory mismatch at %#llx: expected=%02x actual=%02x\n",
                   static_cast<unsigned long long>(region_.address + offset),
                   std::to_integer<unsigned>(expected[offset]),
                   std::to_integer<unsigned>(actual_[offset]));
      fail(test_name, engine, instruction, "unexpected data-region contents");
    }
  }

private:
  MemoryRegion region_;
  ScratchBacking backing_;
  Mapping mapping_;
  std::vector<std::byte> actual_;
};

[[nodiscard]] bool ranges_overlap(std::uint64_t first_address,
                                  std::size_t first_size,
                                  std::uint64_t second_address,
                                  std::size_t second_size) noexcept {
  const auto first_end = first_address + first_size;
  const auto second_end = second_address + second_size;
  return first_address < second_end && second_address < first_end;
}

void validate_memory_regions(std::string_view name, std::uint32_t instruction,
                             std::span<const MemoryRegion> memory) {
  for (std::size_t index = 0; index < memory.size(); ++index) {
    const auto &region = memory[index];
    const auto size = region.initial.size();
    if (size == 0 || size % DARWIN_UML_GUEST_PAGE_SIZE != 0 ||
        region.address % DARWIN_UML_GUEST_PAGE_SIZE != 0 ||
        region.address > std::numeric_limits<std::uint64_t>::max() - size ||
        (!region.expected.empty() &&
         region.expected.size() != region.initial.size()))
      fail(name, "setup", instruction, "invalid memory-region shape");
    if (ranges_overlap(region.address, size, code_base,
                       DARWIN_UML_GUEST_PAGE_SIZE))
      fail(name, "setup", instruction, "memory region overlaps code");
    for (std::size_t prior = 0; prior < index; ++prior)
      if (ranges_overlap(region.address, size, memory[prior].address,
                         memory[prior].initial.size()))
        fail(name, "setup", instruction, "memory regions overlap");
  }
}

[[nodiscard]] unsigned long next_generation() noexcept {
  static std::atomic<unsigned long> generation{0x10000};
  return generation.fetch_add(1, std::memory_order_relaxed);
}

void install_program(
    std::string_view name, std::uint32_t diagnostic_instruction,
    std::span<const ProgramWord> code,
    std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> &page,
    std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> &installed_page) {
  page.fill(std::byte{});
  std::array<bool, DARWIN_UML_GUEST_PAGE_SIZE / sizeof(std::uint32_t)> used{};
  if (code.empty())
    fail(name, "setup", diagnostic_instruction, "empty test program");
  for (const auto word : code) {
    if (word.offset % sizeof(word.instruction) != 0 ||
        word.offset > page.size() - sizeof(word.instruction) ||
        used[word.offset / sizeof(word.instruction)])
      fail(name, "setup", diagnostic_instruction,
           "invalid test-program layout");
    used[word.offset / sizeof(word.instruction)] = true;
    std::memcpy(page.data() + word.offset, &word.instruction,
                sizeof(word.instruction));
  }
  std::size_t first = 0;
  while (first != page.size() && page[first] == installed_page[first])
    ++first;
  if (first == page.size())
    return;
  std::size_t end = page.size();
  while (end != first && page[end - 1] == installed_page[end - 1])
    --end;
  if (darwin_uml_static_vm_copy_from_buffer(
          code_base + first, page.data() + first, end - first) != 0)
    fail(name, "restore", diagnostic_instruction, "cannot restore code page");
  installed_page = page;
}

[[nodiscard]] Outcome run_engine(const arm64::CpuState &initial,
                                 StaticGuestMemory &memory,
                                 unsigned long generation,
                                 ::uml::cpu::CpuEngine &engine) {
  Outcome outcome{};
  outcome.cpu = initial;
  const auto sequences = darwin_uml_static_vm_sequences(generation);
  memory.prepare(generation, sequences.mapping_changes);
  ::uml::cpu::ExecutionRequest request{
      .cpu = outcome.cpu,
      .memory = memory,
      .translated_memory = &memory,
      .generation = generation,
      .instruction_budget = execution_limit,
  };
  const auto result = engine.run(request);
  outcome.event = event_for(result.reason);
  outcome.instructions = result.retired_instructions;
  outcome.fault = {result.fault.address, result.fault.access};
  outcome.syndrome = result.syndrome;
  return outcome;
}

} // namespace

bool host_supports(NativeRequirements requirements) noexcept {
  return ::uml::platform::apple::probed_host_capabilities().supports(requirements.features());
}

void require_architecture_equal(const arm64::CpuState &expected,
                                const arm64::CpuState &actual,
                                std::string_view case_name,
                                std::string_view engine,
                                std::uint32_t instruction,
                                const std::source_location location) {
  if (const auto difference = first_architecture_difference(expected, actual))
    fail(case_name, engine, instruction, *difference, location);
}

class EngineHarness::Implementation final {
public:
  Implementation()
      : generation_{next_generation()},
        address_space_{generation_},
        backing_{backing_name, DARWIN_UML_GUEST_PAGE_SIZE},
        mapping_{generation_,
                 code_base,
                 backing_,
                 DARWIN_UML_GUEST_PAGE_SIZE,
                 PROT_READ | PROT_EXEC,
                 true,
                 false} {
    const ::uml::cpu::EngineConfiguration configuration{
        .host_capabilities = ::uml::platform::apple::probed_host_capabilities(),
    };
    gadget_engine_ = ::uml::cpu::make_cpu_engine(
        ::uml::cpu::EngineKind::gadgets, configuration);
    jit_engine_ = ::uml::cpu::make_cpu_engine(
        ::uml::cpu::EngineKind::jit, configuration);
  }

  void check(const InstructionCase &test_case,
             const std::source_location &location) {
    const std::array code{
        ProgramWord{.offset = 0, .instruction = test_case.instruction},
        ProgramWord{.offset = sizeof(std::uint32_t),
                    .instruction = terminal_system_call},
    };
    auto expected = test_case.expected;
    const bool direct_system_call =
        test_case.event == ExpectedEvent::system_call &&
        (test_case.instruction & 0xffe0'001fU) == 0xd400'0001U;
    expected.pc =
        test_case.event == ExpectedEvent::system_call
            ? code_base + (direct_system_call ? 1U : 2U) *
                              sizeof(std::uint32_t)
            : code_base;
    check_program({
        .name = test_case.name,
        .code = std::span<const ProgramWord>{code},
        .initial = test_case.initial,
        .expected = expected,
        .event = test_case.event,
        .native = test_case.native,
        .fault = test_case.fault,
        .compare_fault = test_case.compare_fault,
        .syndrome = test_case.syndrome,
        .invalid_encoding = test_case.invalid_encoding,
        .memory = test_case.memory,
        .validator = test_case.validator,
        .execution_context = test_case.execution_context,
        .before_engine = test_case.before_engine,
        .after_engine = test_case.after_engine,
        .expected_instructions = instruction_count_expectation(test_case),
    }, location);
  }

  void check_program(const ProgramCase &test_case,
                     const std::source_location &location) {
    if (test_case.entry_offset % sizeof(std::uint32_t) != 0 ||
        test_case.entry_offset >
            DARWIN_UML_GUEST_PAGE_SIZE - sizeof(std::uint32_t))
      fail(test_case.name, "setup", 0, "invalid entry offset", location);
    std::uint32_t diagnostic_instruction = 0;
    bool found_entry = false;
    for (const auto word : test_case.code)
      if (word.offset == test_case.entry_offset) {
        diagnostic_instruction = word.instruction;
        found_entry = true;
        break;
      }
    if (!found_entry)
      fail(test_case.name, "setup", 0, "entry instruction is absent", location);

    validate_memory_regions(test_case.name, diagnostic_instruction,
                            test_case.memory);
    std::vector<std::unique_ptr<CaseRegion>> regions;
    regions.reserve(test_case.memory.size());
    for (const auto &region : test_case.memory)
      regions.push_back(std::make_unique<CaseRegion>(generation_, region));

    std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> expected_page{};
    install_program(test_case.name, diagnostic_instruction, test_case.code,
                    expected_page, installed_page_);

    auto initial = test_case.initial;
    initial.pc = code_base + test_case.entry_offset;
    const auto &expected = test_case.expected;
    std::optional<std::size_t> gadget_instructions;

    for (const auto engine : {Engine::gadgets, Engine::jit}) {
      if (!engine_available(engine)) {
        if (engine != Engine::jit)
          fail(test_case.name, "setup", diagnostic_instruction,
               "required CPU engine is unavailable", location);
        ++counts_.jit_skipped;
        ++counts_.jit_unavailable;
        continue;
      }
      if (engine == Engine::jit && !test_case.invalid_encoding &&
          !host_supports(test_case.native)) {
        ++counts_.jit_skipped;
        ++counts_.jit_skipped_host_feature;
        continue;
      }

      for (const auto &region : regions)
        region->restore();
      void *translated = nullptr;
      if (darwin_uml_static_vm_translate(
              generation_, code_base, DARWIN_UML_STATIC_EXECUTE, &translated) !=
              DARWIN_UML_STATIC_TRANSLATED ||
          translated == nullptr)
        fail(test_case.name, engine_name(engine), diagnostic_instruction,
             "code mapping disappeared", location);
      StaticGuestMemory memory;
      const auto sequences = darwin_uml_static_vm_sequences(generation_);
      memory.prepare(generation_, sequences.mapping_changes);
      const auto current_engine = engine_name(engine);
      if (test_case.before_engine != nullptr)
        test_case.before_engine(
            test_case.execution_context, current_engine, generation_);
      auto *selected_engine = engine_instance(engine);
      if (selected_engine == nullptr)
        fail(test_case.name, "setup", diagnostic_instruction,
             "selected CPU engine was not constructed", location);
      const auto outcome =
          run_engine(initial, memory, generation_, *selected_engine);
      switch (engine) {
      case Engine::gadgets:
        ++counts_.gadgets;
        break;
      case Engine::jit:
        ++counts_.jit;
        break;
      }
      if (test_case.after_engine != nullptr)
        test_case.after_engine(
            test_case.execution_context, current_engine, generation_);

      /* The public engine boundary owns translated host-state restoration and
       * executor cleanup. Do not repair leaked state in this common harness:
       * doing so would let the full instruction matrix pass while a direct
       * production caller observes an active guest FP environment or a stale
       * indirect-link marker. */
      if (outcome.cpu.gadget_fp_active ||
          outcome.cpu.gadget_indirect_miss != nullptr)
        fail(test_case.name, current_engine, diagnostic_instruction,
             "engine-private execution state escaped the public boundary",
             location);

      if (outcome.event != test_case.event) {
        std::fprintf(stderr,
                     "expected event %.*s, actual event %.*s, "
                     "instructions=%zu fault=%#llx/%u\n",
                     static_cast<int>(event_name(test_case.event).size()),
                     event_name(test_case.event).data(),
                     static_cast<int>(event_name(outcome.event).size()),
                     event_name(outcome.event).data(), outcome.instructions,
                     static_cast<unsigned long long>(outcome.fault.address),
                     static_cast<unsigned>(outcome.fault.access));
        fail(test_case.name, current_engine, diagnostic_instruction,
             "unexpected terminal event", location);
      }

      if (test_case.variable_instruction_count) {
        /* Nothing to compare: see ProgramCase. */
      } else if (test_case.expected_instructions.has_value()) {
        if (outcome.instructions != *test_case.expected_instructions) {
          std::fprintf(stderr,
                       "expected instructions=%zu actual instructions=%zu\n",
                       *test_case.expected_instructions,
                       outcome.instructions);
          fail(test_case.name, current_engine, diagnostic_instruction,
               "retired instruction count", location);
        }
      } else if (test_case.event != ExpectedEvent::instruction_budget &&
                 engine == Engine::gadgets) {
        gadget_instructions = outcome.instructions;
      } else if (test_case.event != ExpectedEvent::instruction_budget &&
                 gadget_instructions.has_value() &&
                 outcome.instructions != *gadget_instructions) {
        std::fprintf(stderr,
                     "gadget instructions=%zu actual instructions=%zu\n",
                     *gadget_instructions, outcome.instructions);
        fail(test_case.name, current_engine, diagnostic_instruction,
             "engine instruction-count mismatch", location);
      }

      /*
       * Deferred state is part of the engine-entry contract. Do not
       * materialize it before execution; normalize copies only after the
       * engine has finished so architecture comparisons remain bit-exact.
       */
      auto compared_initial = initial;
      auto compared_expected = expected;
      auto compared_actual = outcome.cpu;
      compared_initial.access_fp_state();
      compared_expected.access_fp_state();
      compared_actual.access_fp_state();
      if (test_case.validator != nullptr)
        test_case.validator(compared_initial, compared_expected,
                            compared_actual, test_case.name, current_engine,
                            diagnostic_instruction);
      else
        require_architecture_equal(compared_expected, compared_actual,
                                   test_case.name, current_engine,
                                   diagnostic_instruction, location);
      if (test_case.compare_fault &&
          (outcome.fault.address != test_case.fault.address ||
           outcome.fault.access != test_case.fault.access)) {
        std::fprintf(
            stderr,
            "expected fault=%#llx/%u actual fault=%#llx/%u\n",
            static_cast<unsigned long long>(test_case.fault.address),
            static_cast<unsigned>(test_case.fault.access),
            static_cast<unsigned long long>(outcome.fault.address),
            static_cast<unsigned>(outcome.fault.access));
        fail(test_case.name, current_engine, diagnostic_instruction,
             "fault metadata", location);
      }
      if (test_case.event == ExpectedEvent::floating_point_trap &&
          outcome.syndrome != test_case.syndrome) {
        std::fprintf(stderr, "expected syndrome=%#x actual=%#x\n",
                     test_case.syndrome, outcome.syndrome);
        fail(test_case.name, current_engine, diagnostic_instruction,
             "floating-point syndrome", location);
      }

      if (std::memcmp(translated, expected_page.data(),
                      expected_page.size()) != 0)
        fail(test_case.name, current_engine, diagnostic_instruction,
             "unexpected code-page write", location);
      for (const auto &region : regions)
        region->require_expected(test_case.name, current_engine,
                                 diagnostic_instruction);
    }
  }

  [[nodiscard]] const EngineTestCounts &counts() const noexcept {
    return counts_;
  }

private:
  [[nodiscard]] ::uml::cpu::CpuEngine *
  engine_instance(Engine engine) noexcept {
    switch (engine) {
    case Engine::gadgets:
      return gadget_engine_.get();
    case Engine::jit:
      return jit_engine_.get();
    }
    return nullptr;
  }

  unsigned long generation_;
  AddressSpace address_space_;
  ScratchBacking backing_;
  Mapping mapping_;
  std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> installed_page_{};
  std::unique_ptr<::uml::cpu::CpuEngine> gadget_engine_;
  std::unique_ptr<::uml::cpu::CpuEngine> jit_engine_;
  EngineTestCounts counts_;
};

EngineHarness::EngineHarness()
    : implementation_{std::make_unique<Implementation>()} {}

EngineHarness::~EngineHarness() = default;

void EngineHarness::check(const InstructionCase &test_case,
                          const std::source_location location) {
  implementation_->check(test_case, location);
}

void EngineHarness::check_program(const ProgramCase &test_case,
                                  const std::source_location location) {
  implementation_->check_program(test_case, location);
}

void EngineHarness::check_one(std::string_view name, std::uint32_t instruction,
                              const arm64::CpuState &initial,
                              const arm64::CpuState &expected,
                              NativeRequirements native,
                              const std::source_location location) {
  check({
      .name = name,
      .instruction = instruction,
      .initial = initial,
      .expected = expected,
      .native = native,
  }, location);
}

void EngineHarness::check_illegal(std::string_view name,
                                  std::uint32_t instruction,
                                  const arm64::CpuState &initial,
                                  NativeRequirements native,
                                  const std::source_location location) {
  check({
      .name = name,
      .instruction = instruction,
      .initial = initial,
      .expected = initial,
      .event = ExpectedEvent::illegal_instruction,
      .native = native,
      .invalid_encoding = true,
  }, location);
}

const EngineTestCounts &EngineHarness::counts() const noexcept {
  return implementation_->counts();
}

void EngineHarness::print_summary(std::string_view suite) const {
  const auto &value = counts();
  std::printf("%.*s: gadgets=%zu jit=%zu "
              "jit-skipped-by-host-feature=%zu jit-unavailable=%zu\n",
              static_cast<int>(suite.size()), suite.data(), value.gadgets,
              value.jit, value.jit_skipped_host_feature, value.jit_unavailable);
}

} // namespace uml::emulator::tests
