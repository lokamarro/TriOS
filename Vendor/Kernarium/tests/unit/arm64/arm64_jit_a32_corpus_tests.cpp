// SPDX-License-Identifier: GPL-2.0-or-later
/* The AArch32 instruction corpus on the JIT: every case of the pack that the
 * JIT's level implements runs through CpuEngine::run_aarch32 on the oracle's
 * memory map, and its final registers, APSR, TPIDRs, data bytes, event, fault
 * address and retired count are compared with what the oracle observed.
 * tests/unit/arm64/aarch32_corpus/README.md owns the pack and its format.
 *
 * A store-exclusive may fail without cause, as the architecture allows, and
 * the JIT's is the host's: the host's monitor ends at any interrupt or
 * preemption between the load and the store, and the first fused pair a
 * process runs can lose it too. The oracle's never fails and a guest loops,
 * but a corpus case is one attempt. A case whose only difference from the
 * pack is that failed store-exclusive (the status register 1 where the pack
 * has 0, and the store it therefore did not make) runs again, up to
 * kExclusiveAttempts times in all, and must then agree completely.
 * --interrupt aims a signal storm at the thread that runs the guest so that
 * this happens on purpose: it repeats the selected records, at most --passes
 * N times, until kReservationsWanted cases have lost a reservation, and fails
 * if none did. --attempts N replaces the limit (1 makes the storm fail).
 *
 * The corpus also holds every lowered form out of the slow helper from a
 * clean state: each event-free case runs twice (the first run warms the
 * translation and the TLB), and past the first run one case in which the
 * form always executes must take no helper exit (the others cross a page or
 * write back over the loaded register). The forms the semantic library runs
 * (the parallel arithmetic and the Armv8 extensions the compiler does not
 * lower) must take it in all of them, and every vector form of the simd level
 * must have a lowering. A case whose form is of an extension the host lacks
 * (BFloat16 and the 8-bit matrix forms on an M1) is skipped, because the JIT
 * decodes it as UNDEFINED there. The VJCVT cases include a flush-to-zero
 * subnormal input, which exercises the correction for a host whose FJCVTZS
 * reports it as exact.
 *
 *   uml_arm64_jit_a32_corpus_tests PACK [--record NAME] [--case NAME]
 *       [--form PREFIX] [--list-failures] [--passes N] [--interrupt]
 *       [--attempts N]
 */
#include "aarch32_corpus_cpu_state.hpp"
#include "aarch32_corpus_pack.hpp"
#include "arm64_jit_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/semantics_exit_counters.hpp"
#include "cpu/arm64/jit/a32/a32_compiler.hpp"
#include "cpu/arm64/jit/a32/a32_operations.hpp"
#include "cpu/arm64/jit/a32/a32_vector_operations.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <pthread.h>

namespace {

/* How many times a case whose store-exclusive lost its reservation may run.
 * A lost reservation is an interrupt that landed in a window of tens of
 * nanoseconds, so a few attempts are enough; an engine whose store-exclusive
 * never succeeds fails every one. */
constexpr unsigned kExclusiveAttempts = 16;
/* How many cases an interrupt storm must have cost a reservation before it
 * stops: the losses are a few in a thousand passes, and a storm that has
 * seen some has measured the rerun. */
constexpr std::size_t kReservationsWanted = 4;

using namespace ::uml::emulator::tests;
using namespace ::uml::emulator::tests::a32;
namespace dec = ::uml::cpu::arm64::aarch32;
using ::uml::cpu::ExitReason;

using namespace ::uml::testing::aarch32_corpus;

/* The oracle's exit stubs load the address of one of its two epilogues into
 * the PC. Here each epilogue is an svc, which ends the run, and the epilogue
 * reached tells whether the program finished in A32 or in Thumb state (the
 * Thumb stub's target has bit 0 clear, so both epilogues run in A32 state). */

// --- The machine ----------------------------------------------------------------

class Machine final {
public:
  explicit Machine(unsigned long generation)
      : generation_{generation}, code_{kCodeSize / guest_page},
        data_{kDataSize / guest_page}, read_only_{kReadOnlySize / guest_page},
        engine_{::uml::cpu::make_cpu_engine(::uml::cpu::EngineKind::jit)} {
    require(darwin_uml_static_vm_activate(generation_) == 0);
    map(kCodeBase, code_, kCodeSize, PROT_READ | PROT_WRITE | PROT_EXEC);
    map(kDataBase, data_, kDataSize, PROT_READ | PROT_WRITE);
    map(kReadOnlyBase, read_only_, kReadOnlySize, PROT_READ);
    const auto fill = read_only_fill();
    read_only_.put(0, std::as_bytes(std::span{fill}));
    require(engine_ != nullptr);
  }
  ~Machine() { require(darwin_uml_static_vm_destroy(generation_) == 0); }
  Machine(const Machine &) = delete;
  Machine &operator=(const Machine &) = delete;

  /* Lays the case out; returns whether a nop precedes the exit stub that ends
   * the program. */
  bool load(const Case &test) {
    bool padded = false;
    const auto code = code_image(test, 0xef0000ffU, padded);
    code_.put(0, std::as_bytes(std::span{code}));
    /* The program replaces the last case's: retire the blocks translated
     * from it. */
    require(darwin_uml_static_vm_invalidate_code_generation(
                generation_, kCodeBase, kCodeSize) == 0);
    initial_data_ = data_before(test);
    data_.put(0, std::as_bytes(std::span{initial_data_}));
    return padded;
  }

  /* The data the loaded case started from. */
  [[nodiscard]] const std::vector<std::uint8_t> &initial_data() const {
    return initial_data_;
  }
  [[nodiscard]] std::vector<std::uint8_t> read_data() const {
    std::vector<std::uint8_t> bytes(kDataSize);
    data_.get(0, std::as_writable_bytes(std::span{bytes}));
    return bytes;
  }

  ::uml::cpu::ExecutionResult run(arm64::CpuState &cpu) {
    ::uml::memory::StaticGuestMemory memory;
    const auto sequences = darwin_uml_static_vm_sequences(generation_);
    memory.prepare(generation_, sequences.mapping_changes);
    ::uml::cpu::ExecutionRequest request{
        .cpu = cpu,
        .memory = memory,
        .translated_memory = &memory,
        .generation = generation_,
        .instruction_budget = 4096,
    };
    return engine_->run_aarch32(request);
  }

private:
  void map(std::uint32_t address, JitBacking &backing, std::size_t size, int prot) {
    require(darwin_uml_static_vm_map_generation(
                generation_, reinterpret_cast<void *>(std::uintptr_t{address}),
                backing.descriptor(), 0, size, prot, 1, 1) == 0);
  }

  unsigned long generation_;
  JitBacking code_;
  JitBacking data_;
  JitBacking read_only_;
  std::unique_ptr<::uml::cpu::CpuEngine> engine_;
  std::vector<std::uint8_t> initial_data_;
};

// --- Checking -------------------------------------------------------------------

struct Totals final {
  std::size_t run = 0, skipped = 0, failed = 0;
  /* Cases that agreed with the pack only on a later attempt, after a
   * store-exclusive lost its reservation. */
  std::size_t retried = 0;
  std::map<std::string, std::size_t> failed_forms;
};

/* Host interrupts on demand: a thread aims SIGUSR1 at the thread that runs
 * the guest as fast as the host can raise it. Each delivery enters and leaves
 * the kernel on the interrupted thread, which ends the host's exclusive
 * monitor as a timer tick, an inter-processor interrupt or a preemption does
 * on a loaded host. The handler does nothing. */
class InterruptStorm final {
public:
  InterruptStorm() : target_{::pthread_self()} {
    struct sigaction action {};
    action.sa_handler = [](int) {};
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    require(::sigaction(SIGUSR1, &action, &previous_) == 0);
    thread_ = std::thread{[this] {
      while (!stop_.load(std::memory_order_relaxed))
        (void)::pthread_kill(target_, SIGUSR1);
    }};
  }
  ~InterruptStorm() {
    stop_.store(true, std::memory_order_relaxed);
    thread_.join();
    (void)::sigaction(SIGUSR1, &previous_, nullptr);
  }
  InterruptStorm(const InterruptStorm &) = delete;
  InterruptStorm &operator=(const InterruptStorm &) = delete;

private:
  pthread_t target_;
  struct sigaction previous_ {};
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

/* What one run of a case found. */
struct Attempt final {
  Attempt() = default;
  Attempt(std::string reason) : problem{std::move(reason)} {}
  Attempt(const char *reason) : problem{reason} {}
  /* The reason the JIT disagrees with the oracle, or nothing. */
  std::string problem;
  /* The only difference is a store-exclusive that reported failure where the
   * oracle's succeeded, and the store it therefore did not make. */
  bool reservation_lost = false;
};

[[nodiscard]] bool is_store_exclusive(const Case &test) {
  return test.form.starts_with("STREX") || test.form.starts_with("STLEX");
}

/* Whether the disagreement at register `index` is the whole of it: the
 * store-exclusive's status reads 1 where the pack has 0, and nothing else
 * differs, the data being what the case started from. */
[[nodiscard]] bool lost_reservation(const Machine &machine, const Case &test,
                                    const arm64::CpuState &cpu,
                                    unsigned index) {
  if (!is_store_exclusive(test) || test.final.r[index] != 0U ||
      cpu.registers[index] != 1U)
    return false;
  if (apsr_of(cpu) != test.final.apsr || fpscr_of(cpu) != test.final.fpscr ||
      cpu.a32_tpidruro != test.final.tpidruro || cpu.tls != test.final.tpidrurw)
    return false;
  for (unsigned d = 0; d < kVectors; ++d) {
    std::uint64_t value;
    std::memcpy(&value, cpu.vectors[d / 2].bytes.data() + 8U * (d & 1U), 8);
    if (value != test.final.d[d])
      return false;
  }
  return machine.read_data() == machine.initial_data();
}

/* Runs one attempt at a case. */
Attempt check(Machine &machine, const Case &test) {
  const bool thumb = test.flags & kFlagThumb;
  const bool padded = machine.load(test);
  auto cpu = cpu_from(test.initial, thumb, kCodeBase + test.origin);
  const auto result = machine.run(cpu);

  char text[512];
  const auto fail = [&](const char *what) {
    std::snprintf(text, sizeof text, "%s (reason %d, retired %zu, pc %016llx)",
                  what, static_cast<int>(result.reason), result.retired_instructions,
                  static_cast<unsigned long long>(cpu.pc));
    return std::string{text};
  };

  const std::uint32_t pc = key::key_pc(cpu.pc);
  const bool at_thumb = key::key_is_thumb(cpu.pc);
  unsigned event = 0;
  std::size_t retired = result.retired_instructions;
  switch (result.reason) {
  case ExitReason::system_call: {
    // pc is past the svc: a stub's svc is a finished program.
    const std::uint32_t svc = pc - (at_thumb ? 2U : 4U);
    if (!at_thumb && (svc == kArmEpilogue || svc == kThumbEpilogue)) {
      event = 0;
      if (retired < 2)
        return fail("the stub exit retired too little");
      retired -= 2; // the stub's load of the PC and its svc
      if (padded && (test.flags & kFlagStraight) != 0)
        --retired; // the nop that aligns a Thumb stub the program fell into
      if (((test.flags & kFlagExitThumb) != 0) != (svc == kThumbEpilogue))
        return fail("finished in the wrong instruction set");
    } else {
      event = kEventSvc;
      --retired; // the program's own svc retires but the corpus does not count it
    }
    break;
  }
  case ExitReason::illegal_instruction: {
    event = kSigIll;
    /* The three undefined encodings Linux turns into SIGTRAP (arm64's compat
     * layer, as arm's): A32 0xe7f001f0, T16 0xde01 and T32 0xf7f0a000. */
    std::uint32_t word = 0;
    const std::uint32_t at = pc;
    const std::uint32_t program = kCodeBase + test.origin;
    const auto read16 = [&](std::uint32_t address) {
      const std::uint32_t index = address - program;
      return static_cast<std::uint32_t>(test.halves[index / 2]);
    };
    if (at >= program && at - program < 2 * test.halves.size()) {
      if (at_thumb) {
        word = read16(at);
        if (word == 0xde01U)
          event = kSigTrap;
        else if (word == 0xf7f0U && at + 2 - program < 2 * test.halves.size() &&
                 read16(at + 2) == 0xa000U)
          event = kSigTrap;
      } else if (at + 4 - program <= 2 * test.halves.size()) {
        word = read16(at) | read16(at + 2) << 16;
        if (word == 0xe7f001f0U)
          event = kSigTrap;
      }
    }
    break;
  }
  case ExitReason::breakpoint:
    event = kSigTrap;
    --retired; // a breakpoint retires in the engines' accounting, not in the corpus
    break;
  case ExitReason::memory_fault:
    event = result.fault.alignment ? kSigBus : kSigSegv;
    break;
  case ExitReason::pc_alignment: event = kSigBus; break;
  default:
    return fail("unexpected exit reason");
  }
  if (event != test.event) {
    std::snprintf(text, sizeof text, "event %u, expected %u (reason %d, retired %zu)",
                  event, test.event, static_cast<int>(result.reason),
                  result.retired_instructions);
    return text;
  }
  if (retired != test.retired) {
    std::snprintf(text, sizeof text, "retired %zu (engine %zu), expected %u", retired,
                  result.retired_instructions, test.retired);
    return text;
  }
  if (event == kSigIll || event == kSigTrap) {
    if (pc != test.fault_address) {
      std::snprintf(text, sizeof text, "fault pc %08x, expected %08x", pc,
                    test.fault_address);
      return text;
    }
  } else if (event == kSigSegv || event == kSigBus) {
    if (result.fault.address != test.fault_address) {
      std::snprintf(text, sizeof text, "fault address %08llx, expected %08x",
                    static_cast<unsigned long long>(result.fault.address),
                    test.fault_address);
      return text;
    }
  }
  unsigned differing = 0, first_register = 0;
  for (unsigned index = 0; index < kRegisters; ++index) {
    if (static_cast<std::uint32_t>(cpu.registers[index]) != test.final.r[index] ||
        (cpu.registers[index] >> 32) != 0) {
      if (differing++ == 0)
        first_register = index;
    }
  }
  if (differing != 0) {
    std::snprintf(text, sizeof text, "r%u = %016llx, expected %08x",
                  first_register,
                  static_cast<unsigned long long>(cpu.registers[first_register]),
                  test.final.r[first_register]);
    Attempt attempt{text};
    attempt.reservation_lost =
        differing == 1 && lost_reservation(machine, test, cpu, first_register);
    return attempt;
  }
  if (const auto difference =
          state_difference(cpu, test, machine.read_data(),
                           data_after(test, machine.initial_data())))
    return fail(difference->c_str());
  return {};
}

/* Whether the JIT's own profile accepts every instruction of the program that
 * the oracle executes: an encoding the oracle executes but the JIT's profile
 * leaves out (the extensions this host lacks, such as BF16 and I8MM on the M1
 * Ultra: the guest is shown none of them and the JIT treats them as
 * UNDEFINED) is not the JIT's to run. */
bool jit_accepts(const Case &test, const dec::Profile &profile) {
  bool accepted = true;
  for_each_instruction(test, profile, [&](const ProgramStep &step) {
    accepted = !step.instruction.legal() ||
               step.decode_at(::uml::cpu::arm64::jit::a32::decode_profile())
                   .legal();
    return accepted;
  });
  return accepted;
}

/* The compiler classifies every vector form the simd level has, so that none
 * reaches the illegal-instruction exit for want of lowering. */
void require_vector_forms_classified() {
  using ::uml::cpu::arm64::jit::a32::Op;
  using ::uml::cpu::arm64::jit::a32::VM;
  for (std::size_t index = 0; index < dec::kFormCount; ++index) {
    const auto form = static_cast<dec::Form>(index);
    if (dec::kForms[index].level != dec::Level::simd ||
        ::uml::cpu::arm64::jit::a32::op_info(form).op != Op::vector)
      continue;
    if (::uml::cpu::arm64::jit::a32::vector_info(form).mnemonic == VM::none) {
      std::fprintf(stderr, "%s is a simd form the compiler does not name\n",
                   dec::kForms[index].name);
      require(false);
    }
  }
}

/* Whether the instruction under test always executes in the case: a
 * straight-line program and an unconditional instruction. */
bool target_always_executes(const Case &test, const dec::Profile &profile) {
  if ((test.flags & kFlagStraight) == 0)
    return false;
  unsigned ordinal = 0;
  bool always = false;
  for_each_instruction(test, profile, [&](const ProgramStep &step) {
    if (ordinal++ != test.target)
      return true;
    always = step.instruction.cond >= 14U;
    return false;
  });
  return always;
}

/* The library-exit gate: a form the compiler lowers stays out of the slow
 * helper from a clean state. Every event-free case of a form runs twice (the
 * first run warms the translation and the TLB) and the second run's helper
 * exits for the form under test are counted. */
struct ExitTally final {
  /* Every event-free case, and those that took no helper exit. */
  std::size_t cases = 0, clean = 0;
  /* The same over the cases whose instruction under test always executes. */
  std::size_t executed = 0, executed_clean = 0;
};

std::map<std::string, ExitTally> measure_helper_exits(
    Machine &machine, const Pack &pack, const dec::Profile &profile,
    bool report) {
  std::map<std::string, dec::Form> forms;
  for (std::size_t index = 0; index < dec::kFormCount; ++index)
    forms.emplace(dec::kForms[index].name, static_cast<dec::Form>(index));
  std::map<std::string, ExitTally> tallies;
  ::uml::cpu::arm64::aarch32::count_a32_library_exits(true);
  for (const auto &record : pack.records) {
    if ((record.profiles & kProfileBit) == 0)
      continue;
    for (const auto &test : record.cases) {
      if (test.event != 0 || !jit_accepts(test, profile))
        continue;
      const auto form = forms.find(test.form);
      require(form != forms.end());
      const bool thumb = test.flags & kFlagThumb;
      std::size_t before = 0;
      for (unsigned pass = 0; pass < 2; ++pass) {
        machine.load(test);
        auto cpu = cpu_from(test.initial, thumb, kCodeBase + test.origin);
        before = ::uml::cpu::arm64::aarch32::a32_library_exits(form->second);
        (void)machine.run(cpu);
      }
      const auto taken =
          ::uml::cpu::arm64::aarch32::a32_library_exits(form->second) - before;
      auto &tally = tallies[test.form];
      ++tally.cases;
      tally.clean += taken == 0 ? 1U : 0U;
      if (target_always_executes(test, profile)) {
        ++tally.executed;
        tally.executed_clean += taken == 0 ? 1U : 0U;
      }
      if (report && taken != 0)
        std::fprintf(stderr, "exit %s %s/%s: %zu\n", test.form.c_str(),
                     record.name.c_str(), test.name.c_str(),
                     static_cast<std::size_t>(taken));
    }
  }
  ::uml::cpu::arm64::aarch32::count_a32_library_exits(false);
  return tallies;
}

/* Requires every form the compiler lowers to stay out of the helper from a
 * clean state: some case in which it always executes takes no helper exit
 * (the others cross a page, write back over the loaded register and so on).
 * The forms the library always runs take it every time, which keeps the
 * predicate that names it honest, and the exclusives are measured by their
 * own test. */
void require_lowered_forms_stay_inline(Machine &machine, const Pack &pack,
                                       const dec::Profile &profile) {
  const auto tallies = measure_helper_exits(machine, pack, profile, false);
  std::size_t inline_forms = 0, helper_forms = 0;
  for (const auto &[name, tally] : tallies) {
    dec::Form form{};
    for (std::size_t index = 0; index < dec::kFormCount; ++index)
      if (name == dec::kForms[index].name)
        form = static_cast<dec::Form>(index);
    const auto op = ::uml::cpu::arm64::jit::a32::op_info(form).op;
    using ::uml::cpu::arm64::jit::a32::Op;
    if (::uml::cpu::arm64::jit::a32::is_exclusive_op(op))
      continue;
    /* The cases that decide: those in which the instruction executes, or all
     * of them for a form whose cases are all conditional. */
    const auto decisive = tally.executed != 0 ? tally.executed : tally.cases;
    const auto decisive_clean =
        tally.executed != 0 ? tally.executed_clean : tally.clean;
    if (::uml::cpu::arm64::jit::a32::is_library_form(form)) {
      if (decisive_clean != 0) {
        std::fprintf(stderr, "%s runs inline but is named a library form\n",
                     name.c_str());
        require(false);
      }
      ++helper_forms;
      continue;
    }
    if (decisive_clean == 0) {
      std::fprintf(stderr,
                   "%s is lowered but took the helper from a clean state in "
                   "all %zu cases\n",
                   name.c_str(), decisive);
      require(false);
    }
    ++inline_forms;
  }
  std::printf("%zu lowered forms stay inline from a clean state; %zu run in "
              "the semantic library\n",
              inline_forms, helper_forms);
}

} // namespace

/* --dump WORD [HALFWORD2]: translates one A32 word (or T32 pair) and prints
 * the generated instructions as assembler directives. */
void dump_translation(std::uint32_t word, bool thumb) {
  using namespace ::uml::cpu::arm64::jit::a32;
  A32Compiler compiler;
  std::array<std::byte, 16> bytes{};
  const std::uint32_t nop = 0xe320f000U;
  if (thumb) {
    const std::uint16_t first = static_cast<std::uint16_t>(word >> 16);
    const std::uint16_t second = static_cast<std::uint16_t>(word);
    std::memcpy(bytes.data(), &first, 2);
    std::memcpy(bytes.data() + 2, &second, 2);
  } else {
    std::memcpy(bytes.data(), &word, 4);
    std::memcpy(bytes.data() + 4, &nop, 4);
  }
  Request request;
  request.key = thumb ? ::uml::cpu::arm64::aarch32::t32_key(0x1000)
                      : ::uml::cpu::arm64::aarch32::a32_key(0x1000);
  request.bytes = bytes;
  request.single = true;
  const auto result = compiler.compile(request);
  for (const auto instruction : result.translation.words)
    std::printf(".inst 0x%08x\n", instruction);
}

int main(int argc, char **argv) {
  const char *path = nullptr;
  std::string only_record;
  std::string only_case;
  bool list_failures = false;
  bool trace = false;
  bool all_failures = false;
  bool exit_report = false;
  bool interrupt = false;
  std::size_t passes = 1;
  unsigned attempts_limit = kExclusiveAttempts;
  std::string only_form;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--record" && index + 1 < argc)
      only_record = argv[++index];
    else if (argument == "--case" && index + 1 < argc)
      only_case = argv[++index];
    else if (argument == "--list-failures")
      list_failures = true;
    else if (argument == "--trace")
      trace = true;
    else if (argument == "--all-failures")
      all_failures = true;
    else if (argument == "--exit-report")
      exit_report = true;
    else if (argument == "--form" && index + 1 < argc)
      only_form = argv[++index];
    else if (argument == "--passes" && index + 1 < argc)
      passes = static_cast<std::size_t>(std::strtoul(argv[++index], nullptr, 10));
    else if (argument == "--attempts" && index + 1 < argc)
      attempts_limit =
          static_cast<unsigned>(std::strtoul(argv[++index], nullptr, 10));
    else if (argument == "--interrupt")
      interrupt = true;
    else if (argument == "--dump" && index + 1 < argc) {
      const std::uint32_t word =
          static_cast<std::uint32_t>(std::strtoul(argv[++index], nullptr, 16));
      const bool thumb = index + 1 < argc && std::string_view{argv[index + 1]} == "thumb";
      dump_translation(word, thumb);
      return 0;
    }
    else
      path = argv[index];
  }
  require(path != nullptr && passes != 0 && attempts_limit != 0);
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  require_vector_forms_classified();
  const auto pack = read_pack(read_file(path));
  const dec::Profile profile;

  Machine machine{0x1a32'0300};
  if (exit_report) {
    const auto tallies = measure_helper_exits(machine, pack, profile, true);
    for (const auto &[form, tally] : tallies)
      if (tally.clean != tally.cases)
        std::printf("%s: %zu of %zu cases clean\n", form.c_str(), tally.clean,
                    tally.cases);
    return 0;
  }
  Totals totals;
  std::optional<InterruptStorm> storm;
  if (interrupt)
    storm.emplace();
  for (std::size_t pass = 0; pass < passes; ++pass) {
    for (const auto &record : pack.records) {
      if ((record.profiles & kProfileBit) == 0 ||
          (!only_record.empty() && record.name != only_record))
        continue;
      for (const auto &test : record.cases) {
        if (!only_case.empty() && test.name != only_case)
          continue;
        if (!only_form.empty() && !test.form.starts_with(only_form))
          continue;
        if (!jit_accepts(test, profile)) {
          ++totals.skipped;
          continue;
        }
        ++totals.run;
        if (trace) {
          std::fprintf(stderr, "case %s:", test.name.c_str());
          for (const auto half : test.halves)
            std::fprintf(stderr, " %04x", half);
          std::fprintf(stderr, "\n");
        }
        auto attempt = check(machine, test);
        unsigned attempts = 1;
        while (attempt.reservation_lost && attempts < attempts_limit) {
          attempt = check(machine, test);
          ++attempts;
        }
        if (attempt.problem.empty()) {
          if (attempts > 1)
            ++totals.retried;
          continue;
        }
        ++totals.failed;
        ++totals.failed_forms[test.form];
        if (totals.failed_forms[test.form] == 1 || all_failures ||
            !only_case.empty()) {
          std::fprintf(stderr, "FAIL %s: %s: %s", record.name.c_str(),
                       test.name.c_str(), attempt.problem.c_str());
          if (attempts > 1)
            std::fprintf(stderr, " (%u attempts)", attempts);
          std::fprintf(stderr, "\n  program:");
          for (const auto half : test.halves)
            std::fprintf(stderr, " %04x", half);
          std::fprintf(stderr, "%s\n",
                       test.flags & kFlagThumb ? " (thumb)" : " (arm)");
        }
      }
    }
    if (interrupt && totals.retried + totals.failed >= kReservationsWanted)
      break;
  }
  storm.reset();
  std::printf("AArch32 corpus on the JIT: %zu cases run, %zu skipped below the level, "
              "%zu failed (%zu exclusive retried)\n",
              totals.run, totals.skipped, totals.failed, totals.retried);
  if (list_failures || totals.failed != 0)
    for (const auto &[form, count] : totals.failed_forms)
      std::printf("  %s: %zu\n", form.c_str(), count);
  /* A storm that never ended a reservation proved nothing. */
  if (interrupt && totals.retried == 0 && totals.failed == 0) {
    std::fprintf(stderr, "--interrupt: no store-exclusive lost its "
                         "reservation, so none was interrupted\n");
    return 1;
  }
  if (totals.failed == 0 && only_record.empty() && only_case.empty() &&
      only_form.empty() && !interrupt)
    require_lowered_forms_stay_inline(machine, pack, profile);
  return totals.failed == 0 ? 0 : 1;
}
