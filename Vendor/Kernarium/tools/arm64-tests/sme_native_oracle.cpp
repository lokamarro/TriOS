// SPDX-License-Identifier: GPL-2.0-or-later
//
// Native oracle for the SME corpus cases that tools/arm64-tests/
// sme-native-corpus.py authors. It runs each case of a request file on this
// host's own CPU and writes what the hardware did: the terminal event, the
// PC, the fault address and syndrome, the general, SIMD&FP, SVE, and SME
// registers, and the bytes of every data region. sme-native-corpus.py owns
// the file formats; its module docstring describes them.
//
// A case runs on the calling thread. The thread's Mach exception port, not a
// signal handler, catches its traps: XNU drops the upper bits of the Z
// registers when it delivers a signal in streaming mode, while a thread
// stopped in a Mach exception keeps its whole SME state. A handler thread
// sets the general registers and NZCV through thread_set_state and moves
// the PC between three stages:
//
//   oracle_enter   saves the caller's registers, loads SVCR, ZA, ZT0, Z, P
//                  or V, FPCR, FPSR, and TPIDR2 from the load bank, and
//                  stops at BRK #0x10; the handler then loads X0-X30 and
//                  NZCV and starts the case program through the prologue;
//   the program    runs until it traps; its terminal SVC is replaced with
//                  BRK #7, so a completed case also traps;
//   oracle_store   stores the live SME and SIMD&FP state into the store bank
//                  and stops at BRK #0x11; oracle_restore then leaves
//                  streaming mode and returns to the caller.
//
// XNU sets FPSR to 0x0800009F on every kernel entry while a thread is in
// streaming mode, interrupts included, so FPSR is set and read only in user
// mode, with no trap between the instructions and either access. The last
// words of the code page hold two stubs. The prologue, where the handler
// starts every case, sets FPSR from X0, reloads the case's X0 from a literal,
// and branches to the entry. A run that ends in streaming mode runs again
// with the word it ended at replaced by a branch to the epilogue, which reads
// FPSR into X0 before its own BRK; that value is the case's FPSR. Since an
// interrupt can only leave 0x0800009F, a rerun that reads it runs again, up
// to three more times, and any other value is final. Every rerun must leave
// the state of the first run, but for X0 and a literal load of the replaced
// word.
//
// Only an SME host executes an SME instruction of the template, so the
// oracle also runs requests without SME state on any arm64 Mac, which the
// arm64-sme-native-oracle tooling test does, and so does
// tools/arm64-tests/floating-trap-native-corpus.py for the corpus's trapped
// floating-point cases: XNU reports a trapped floating-point exception as
// EXC_BAD_INSTRUCTION, with the exception syndrome's EC 0x2c.

#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t request_version = 1;
constexpr std::uint32_t response_version = 2;
constexpr std::size_t code_page_size = 16384;
constexpr std::size_t host_page_size = 16384;
constexpr std::uint32_t sentinel = 0xd42000e0;  // BRK #7, in place of SVC #0
constexpr std::uint32_t supervisor_call = 0xd4000001;
constexpr std::uint32_t fill_word = 0xd4200240;  // BRK #0x12
constexpr std::uint32_t max_svl = 256;
constexpr std::uint32_t zt0_bytes = 64;

// The stubs at the end of the code page; no program reaches them.
constexpr std::uint32_t stub_offset = code_page_size - 32;
constexpr std::uint32_t prologue_offset = stub_offset;       // MSR FPSR, X0
constexpr std::uint32_t epilogue_offset = stub_offset + 12;  // MRS X0, FPSR
constexpr std::uint32_t epilogue_trap = stub_offset + 16;    // BRK #0x13
constexpr std::uint32_t literal_offset = stub_offset + 24;   // the case's X0
constexpr std::uint32_t write_fpsr = 0xd51b4420;             // MSR FPSR, X0
constexpr std::uint32_t read_fpsr = 0xd53b4420;              // MRS X0, FPSR
constexpr std::uint32_t load_literal = 0x58000000;           // LDR X0, label
constexpr std::uint32_t branch = 0x14000000;                 // B label
constexpr std::uint32_t epilogue_break = 0xd4200260;         // BRK #0x13
constexpr std::uint32_t streaming_reset_fpsr = 0x0800009f;
constexpr unsigned fpsr_reruns = 4;

// Terminal events, numbered as the corpus pack numbers them.
enum Event : std::uint32_t {
    system_call = 0,
    memory_fault = 1,
    illegal_instruction = 3,
    breakpoint = 4,
    floating_point_trap = 5,
    fatal = 8,
    epilogue = 0x100,  // a rerun's FPSR read; never in a response
};

// Region flags.
constexpr std::uint8_t region_readable = 1;
constexpr std::uint8_t region_writable = 2;
constexpr std::uint8_t region_explicit = 0x80;

// The template's view of a register bank and of a run; the offsets are
// fixed by the assembly below.
struct Bank {
    std::uint64_t svcr;
    std::uint64_t fpcr;
    std::uint64_t fpsr;
    std::uint64_t tpidr2;
    std::uint8_t *v;
    std::uint8_t *z;
    std::uint8_t *p;
    std::uint8_t *za;
    std::uint8_t *zt0;
};
static_assert(offsetof(Bank, svcr) == 0 && offsetof(Bank, fpcr) == 8 &&
              offsetof(Bank, fpsr) == 16 && offsetof(Bank, tpidr2) == 24 &&
              offsetof(Bank, v) == 32 && offsetof(Bank, z) == 40 &&
              offsetof(Bank, p) == 48 && offsetof(Bank, za) == 56 &&
              offsetof(Bank, zt0) == 64);

struct Context {
    std::uint64_t saved[22];  // x19-x30, sp, d8-d15, fpcr
    std::uint64_t sme;
    std::uint64_t sme2;
    std::uint64_t svl;
    Bank *load;
    Bank *store;
};
static_assert(offsetof(Context, sme) == 176 && offsetof(Context, sme2) == 184 &&
              offsetof(Context, svl) == 192 && offsetof(Context, load) == 200 &&
              offsetof(Context, store) == 208);

}  // namespace

extern "C" {
void oracle_enter(Context *context);
extern char oracle_ready[], oracle_store[], oracle_stored[], oracle_restore[];
}

#define UML_ORACLE_Z_LOAD(op, n) "\t" op " z" #n ", [x4, #" #n ", mul vl]\n"
#define UML_ORACLE_P_LOAD(op, n) "\t" op " p" #n ", [x4, #" #n ", mul vl]\n"
#define UML_ORACLE_Z_ALL(op)                                                  \
    UML_ORACLE_Z_LOAD(op, 0) UML_ORACLE_Z_LOAD(op, 1) UML_ORACLE_Z_LOAD(op, 2) \
    UML_ORACLE_Z_LOAD(op, 3) UML_ORACLE_Z_LOAD(op, 4) UML_ORACLE_Z_LOAD(op, 5) \
    UML_ORACLE_Z_LOAD(op, 6) UML_ORACLE_Z_LOAD(op, 7) UML_ORACLE_Z_LOAD(op, 8) \
    UML_ORACLE_Z_LOAD(op, 9) UML_ORACLE_Z_LOAD(op, 10)                        \
    UML_ORACLE_Z_LOAD(op, 11) UML_ORACLE_Z_LOAD(op, 12)                       \
    UML_ORACLE_Z_LOAD(op, 13) UML_ORACLE_Z_LOAD(op, 14)                       \
    UML_ORACLE_Z_LOAD(op, 15) UML_ORACLE_Z_LOAD(op, 16)                       \
    UML_ORACLE_Z_LOAD(op, 17) UML_ORACLE_Z_LOAD(op, 18)                       \
    UML_ORACLE_Z_LOAD(op, 19) UML_ORACLE_Z_LOAD(op, 20)                       \
    UML_ORACLE_Z_LOAD(op, 21) UML_ORACLE_Z_LOAD(op, 22)                       \
    UML_ORACLE_Z_LOAD(op, 23) UML_ORACLE_Z_LOAD(op, 24)                       \
    UML_ORACLE_Z_LOAD(op, 25) UML_ORACLE_Z_LOAD(op, 26)                       \
    UML_ORACLE_Z_LOAD(op, 27) UML_ORACLE_Z_LOAD(op, 28)                       \
    UML_ORACLE_Z_LOAD(op, 29) UML_ORACLE_Z_LOAD(op, 30)                       \
    UML_ORACLE_Z_LOAD(op, 31)
#define UML_ORACLE_P_ALL(op)                                                  \
    UML_ORACLE_P_LOAD(op, 0) UML_ORACLE_P_LOAD(op, 1) UML_ORACLE_P_LOAD(op, 2) \
    UML_ORACLE_P_LOAD(op, 3) UML_ORACLE_P_LOAD(op, 4) UML_ORACLE_P_LOAD(op, 5) \
    UML_ORACLE_P_LOAD(op, 6) UML_ORACLE_P_LOAD(op, 7) UML_ORACLE_P_LOAD(op, 8) \
    UML_ORACLE_P_LOAD(op, 9) UML_ORACLE_P_LOAD(op, 10)                        \
    UML_ORACLE_P_LOAD(op, 11) UML_ORACLE_P_LOAD(op, 12)                       \
    UML_ORACLE_P_LOAD(op, 13) UML_ORACLE_P_LOAD(op, 14)                       \
    UML_ORACLE_P_LOAD(op, 15)
#define UML_ORACLE_Q_ALL(op)                                                  \
    "\t" op " q0, q1, [x4, #0]\n\t" op " q2, q3, [x4, #32]\n"                 \
    "\t" op " q4, q5, [x4, #64]\n\t" op " q6, q7, [x4, #96]\n"                \
    "\t" op " q8, q9, [x4, #128]\n\t" op " q10, q11, [x4, #160]\n"            \
    "\t" op " q12, q13, [x4, #192]\n\t" op " q14, q15, [x4, #224]\n"          \
    "\t" op " q16, q17, [x4, #256]\n\t" op " q18, q19, [x4, #288]\n"          \
    "\t" op " q20, q21, [x4, #320]\n\t" op " q22, q23, [x4, #352]\n"          \
    "\t" op " q24, q25, [x4, #384]\n\t" op " q26, q27, [x4, #416]\n"          \
    "\t" op " q28, q29, [x4, #448]\n\t" op " q30, q31, [x4, #480]\n"

// x0 is the Context throughout; x1 the bank; x9 the SVL; x3 the SVCR.
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".arch_extension sme2\n"
    ".globl _oracle_enter\n"
    "_oracle_enter:\n"
    "\tstp x19, x20, [x0, #0]\n\tstp x21, x22, [x0, #16]\n"
    "\tstp x23, x24, [x0, #32]\n\tstp x25, x26, [x0, #48]\n"
    "\tstp x27, x28, [x0, #64]\n\tstp x29, x30, [x0, #80]\n"
    "\tmov x1, sp\n\tstr x1, [x0, #96]\n"
    "\tstp d8, d9, [x0, #104]\n\tstp d10, d11, [x0, #120]\n"
    "\tstp d12, d13, [x0, #136]\n\tstp d14, d15, [x0, #152]\n"
    "\tmrs x1, fpcr\n\tstr x1, [x0, #168]\n"
    "\tldr x1, [x0, #200]\n"
    "\tldr x9, [x0, #192]\n"
    "\tldr x2, [x0, #176]\n"
    "\tcbz x2, 3f\n"
    "\tldr x3, [x1, #0]\n"
    "\ttbz x3, #0, 1f\n"
    "\tsmstart sm\n"
    "1:\n"
    "\ttbz x3, #1, 2f\n"
    "\tsmstart za\n"
    "\tldr x4, [x1, #56]\n"
    "\tmov w12, #0\n"
    "4:\n"
    "\tldr za[w12, 0], [x4]\n"
    "\tadd x4, x4, x9\n"
    "\tadd w12, w12, #1\n"
    "\tcmp x12, x9\n"
    "\tb.lo 4b\n"
    "\tldr x5, [x0, #184]\n"
    "\tcbz x5, 2f\n"
    "\tldr x4, [x1, #64]\n"
    "\tldr zt0, [x4]\n"
    "2:\n"
    "\ttbz x3, #0, 3f\n"
    "\tldr x4, [x1, #40]\n"
    UML_ORACLE_Z_ALL("ldr")
    "\tldr x4, [x1, #48]\n"
    UML_ORACLE_P_ALL("ldr")
    "\tb 5f\n"
    "3:\n"
    "\tldr x4, [x1, #32]\n"
    UML_ORACLE_Q_ALL("ldp")
    "5:\n"
    "\tldr x4, [x1, #8]\n\tmsr fpcr, x4\n"
    "\tldr x4, [x1, #16]\n\tmsr fpsr, x4\n"
    "\tcbz x2, 6f\n"
    "\tldr x4, [x1, #24]\n\tmsr tpidr2_el0, x4\n"
    "6:\n"
    ".globl _oracle_ready\n"
    "_oracle_ready:\n"
    "\tbrk #0x10\n"
    "\tbrk #0x10\n"
    ".globl _oracle_store\n"
    "_oracle_store:\n"
    "\tldr x1, [x0, #208]\n"
    "\tldr x9, [x0, #192]\n"
    "\tldr x2, [x0, #176]\n"
    "\tcbz x2, 3f\n"
    "\tmrs x3, svcr\n\tstr x3, [x1, #0]\n"
    "\tmrs x4, tpidr2_el0\n\tstr x4, [x1, #24]\n"
    "\ttbz x3, #1, 2f\n"
    "\tldr x4, [x1, #56]\n"
    "\tmov w12, #0\n"
    "4:\n"
    "\tstr za[w12, 0], [x4]\n"
    "\tadd x4, x4, x9\n"
    "\tadd w12, w12, #1\n"
    "\tcmp x12, x9\n"
    "\tb.lo 4b\n"
    "\tldr x5, [x0, #184]\n"
    "\tcbz x5, 2f\n"
    "\tldr x4, [x1, #64]\n"
    "\tstr zt0, [x4]\n"
    "2:\n"
    "\ttbz x3, #0, 3f\n"
    "\tldr x4, [x1, #40]\n"
    UML_ORACLE_Z_ALL("str")
    "\tldr x4, [x1, #48]\n"
    UML_ORACLE_P_ALL("str")
    "\tb 5f\n"
    "3:\n"
    "\tldr x4, [x1, #32]\n"
    UML_ORACLE_Q_ALL("stp")
    "5:\n"
    "\tmrs x4, fpcr\n\tstr x4, [x1, #8]\n"
    "\tmrs x4, fpsr\n\tstr x4, [x1, #16]\n"
    ".globl _oracle_stored\n"
    "_oracle_stored:\n"
    "\tbrk #0x11\n"
    "\tbrk #0x11\n"
    ".globl _oracle_restore\n"
    "_oracle_restore:\n"
    "\tldr x2, [x0, #176]\n"
    "\tcbz x2, 1f\n"
    "\tsmstop\n"
    "\tmsr tpidr2_el0, xzr\n"
    "1:\n"
    "\tldr x1, [x0, #168]\n\tmsr fpcr, x1\n"
    "\tmsr fpsr, xzr\n"
    "\tldp x19, x20, [x0, #0]\n\tldp x21, x22, [x0, #16]\n"
    "\tldp x23, x24, [x0, #32]\n\tldp x25, x26, [x0, #48]\n"
    "\tldp x27, x28, [x0, #64]\n\tldp x29, x30, [x0, #80]\n"
    "\tldr x1, [x0, #96]\n\tmov sp, x1\n"
    "\tldp d8, d9, [x0, #104]\n\tldp d10, d11, [x0, #120]\n"
    "\tldp d12, d13, [x0, #136]\n\tldp d14, d15, [x0, #152]\n"
    "\tret\n");

namespace {

[[noreturn]] void fail(const std::string &message)
{
    std::fprintf(stderr, "sme_native_oracle: %s\n", message.c_str());
    std::exit(1);
}

// Base fills, as the corpus pack defines them (tests/unit/arm64/corpus/
// README.md), so a request names only the registers a case sets.
std::uint64_t base_x(unsigned index)
{
    return 0x5eed000000000000ULL | static_cast<std::uint64_t>(index) << 40 |
           0x01010101ULL * (index + 1);
}

std::uint8_t base_v(unsigned index, unsigned byte)
{
    return static_cast<std::uint8_t>(((16 * index + byte) * 0x9d + 0x3b) & 0xff);
}

std::uint8_t fill_hash(std::uint32_t index)
{
    return static_cast<std::uint8_t>((index * 0x9e3779b1U) >> 24);
}

std::uint8_t region_fill(std::uint32_t seed, std::uint32_t offset)
{
    return fill_hash(seed * 0x01000193U + offset);
}

struct State {
    std::array<std::uint64_t, 32> x{};  // X0-X30, then SP
    std::uint32_t nzcv = 0;
    std::uint32_t fpcr = 0;
    std::uint32_t fpsr = 0;
    std::uint8_t svcr = 0;
    std::uint64_t tpidr2 = 0;
    std::vector<std::uint8_t> v;    // 32 x 16
    std::vector<std::uint8_t> z;    // 32 x svl
    std::vector<std::uint8_t> p;    // 16 x svl/8
    std::vector<std::uint8_t> za;   // svl x svl
    std::vector<std::uint8_t> zt0;  // 64

    void base(std::uint32_t svl)
    {
        for (unsigned i = 0; i < 32; ++i)
            x[i] = base_x(i);
        nzcv = fpcr = fpsr = 0;
        svcr = 0;
        tpidr2 = 0;
        v.resize(32 * 16);
        for (unsigned i = 0; i < 32; ++i)
            for (unsigned b = 0; b < 16; ++b)
                v[16 * i + b] = base_v(i, b);
        z.assign(32 * svl, 0);
        p.assign(16 * svl / 8, 0);
        za.assign(svl * svl, 0);
        zt0.assign(zt0_bytes, 0);
        for (unsigned i = 0; i < 32; ++i)
            for (unsigned b = 0; b < svl; ++b)
                z[i * svl + b] = fill_hash(256 * i + b);
        for (unsigned i = 0; i < 16; ++i)
            for (unsigned b = 0; b < svl / 8; ++b)
                p[i * svl / 8 + b] = fill_hash(0x2000 + 32 * i + b);
        for (unsigned r = 0; r < svl; ++r)
            for (unsigned b = 0; b < svl; ++b)
                za[r * svl + b] = fill_hash(0x10000 + 256 * r + b);
        for (unsigned b = 0; b < zt0_bytes; ++b)
            zt0[b] = fill_hash(0x30000 + b);
    }
};

class Reader {
public:
    explicit Reader(std::vector<std::uint8_t> data) : data_(std::move(data)) {}

    const std::uint8_t *bytes(std::size_t count)
    {
        if (count > data_.size() - offset_)
            fail("truncated request");
        const std::uint8_t *result = data_.data() + offset_;
        offset_ += count;
        return result;
    }

    template <typename T> T get()
    {
        T value;
        std::memcpy(&value, bytes(sizeof(T)), sizeof(T));
        return value;
    }

    void copy(std::uint8_t *target, std::size_t count)
    {
        std::memcpy(target, bytes(count), count);
    }

    bool done() const { return offset_ == data_.size(); }

private:
    std::vector<std::uint8_t> data_;
    std::size_t offset_ = 0;
};

class Writer {
public:
    template <typename T> void put(T value)
    {
        const auto *raw = reinterpret_cast<const std::uint8_t *>(&value);
        data_.insert(data_.end(), raw, raw + sizeof(T));
    }

    void bytes(const std::uint8_t *source, std::size_t count)
    {
        data_.insert(data_.end(), source, source + count);
    }

    const std::vector<std::uint8_t> &data() const { return data_; }

private:
    std::vector<std::uint8_t> data_;
};

// A state is encoded against a reference: X and register-file masks select
// the entries that differ, and the scalar fields are always present.
void read_state(Reader &reader, State &state, std::uint32_t svl)
{
    const auto x_mask = reader.get<std::uint32_t>();
    for (unsigned i = 0; i < 32; ++i)
        if (x_mask >> i & 1)
            state.x[i] = reader.get<std::uint64_t>();
    state.nzcv = reader.get<std::uint32_t>();
    state.fpcr = reader.get<std::uint32_t>();
    state.fpsr = reader.get<std::uint32_t>();
    state.svcr = reader.get<std::uint8_t>();
    state.tpidr2 = reader.get<std::uint64_t>();
    const auto v_mask = reader.get<std::uint32_t>();
    for (unsigned i = 0; i < 32; ++i)
        if (v_mask >> i & 1)
            reader.copy(&state.v[16 * i], 16);
    if (svl == 0) {
        if (state.svcr != 0 || state.tpidr2 != 0)
            fail("SME state in a request without an SVL");
        return;
    }
    const auto z_mask = reader.get<std::uint32_t>();
    for (unsigned i = 0; i < 32; ++i)
        if (z_mask >> i & 1)
            reader.copy(&state.z[i * svl], svl);
    const auto p_mask = reader.get<std::uint16_t>();
    for (unsigned i = 0; i < 16; ++i)
        if (p_mask >> i & 1)
            reader.copy(&state.p[i * svl / 8], svl / 8);
    std::vector<std::uint8_t> rows(svl / 8);
    reader.copy(rows.data(), rows.size());
    for (unsigned r = 0; r < svl; ++r)
        if (rows[r / 8] >> (r % 8) & 1)
            reader.copy(&state.za[r * svl], svl);
    if (reader.get<std::uint8_t>())
        reader.copy(state.zt0.data(), zt0_bytes);
    if (state.svcr & ~3U)
        fail("reserved SVCR bits");
}

void write_state(Writer &writer, const State &state, const State &reference,
                 std::uint32_t svl)
{
    std::uint32_t x_mask = 0;
    for (unsigned i = 0; i < 32; ++i)
        if (state.x[i] != reference.x[i])
            x_mask |= 1U << i;
    writer.put(x_mask);
    for (unsigned i = 0; i < 32; ++i)
        if (x_mask >> i & 1)
            writer.put(state.x[i]);
    writer.put(state.nzcv);
    writer.put(state.fpcr);
    writer.put(state.fpsr);
    writer.put(state.svcr);
    writer.put(state.tpidr2);
    std::uint32_t v_mask = 0;
    for (unsigned i = 0; i < 32; ++i)
        if (std::memcmp(&state.v[16 * i], &reference.v[16 * i], 16) != 0)
            v_mask |= 1U << i;
    writer.put(v_mask);
    for (unsigned i = 0; i < 32; ++i)
        if (v_mask >> i & 1)
            writer.bytes(&state.v[16 * i], 16);
    if (svl == 0)
        return;
    std::uint32_t z_mask = 0;
    for (unsigned i = 0; i < 32; ++i)
        if (std::memcmp(&state.z[i * svl], &reference.z[i * svl], svl) != 0)
            z_mask |= 1U << i;
    writer.put(z_mask);
    for (unsigned i = 0; i < 32; ++i)
        if (z_mask >> i & 1)
            writer.bytes(&state.z[i * svl], svl);
    std::uint16_t p_mask = 0;
    for (unsigned i = 0; i < 16; ++i)
        if (std::memcmp(&state.p[i * svl / 8], &reference.p[i * svl / 8],
                        svl / 8) != 0)
            p_mask = static_cast<std::uint16_t>(p_mask | 1U << i);
    writer.put(p_mask);
    for (unsigned i = 0; i < 16; ++i)
        if (p_mask >> i & 1)
            writer.bytes(&state.p[i * svl / 8], svl / 8);
    std::vector<std::uint8_t> rows(svl / 8);
    for (unsigned r = 0; r < svl; ++r)
        if (std::memcmp(&state.za[r * svl], &reference.za[r * svl], svl) != 0)
            rows[r / 8] = static_cast<std::uint8_t>(rows[r / 8] | 1U << (r % 8));
    writer.bytes(rows.data(), rows.size());
    for (unsigned r = 0; r < svl; ++r)
        if (rows[r / 8] >> (r % 8) & 1)
            writer.bytes(&state.za[r * svl], svl);
    const bool zt0 = state.zt0 != reference.zt0;
    writer.put<std::uint8_t>(zt0);
    if (zt0)
        writer.bytes(state.zt0.data(), zt0_bytes);
}

struct Region {
    std::uint64_t address = 0;
    std::uint32_t size = 0;
    std::uint8_t flags = 0;
    std::vector<std::uint8_t> initial;
};

struct Case {
    std::uint32_t entry = 0;
    std::vector<std::uint32_t> words;
    State initial;
    std::vector<Region> regions;
};

struct Host {
    bool sme = false;
    bool sme2 = false;
    std::uint32_t svl = 0;
    std::uint32_t features = 0;
};

bool sysctl_flag(const char *name)
{
    int value = 0;
    std::size_t size = sizeof(value);
    return sysctlbyname(name, &value, &size, nullptr, 0) == 0 && value != 0;
}

Host probe_host()
{
    Host host;
    host.sme = sysctl_flag("hw.optional.arm.FEAT_SME");
    host.sme2 = host.sme && sysctl_flag("hw.optional.arm.FEAT_SME2");
    static constexpr const char *features[] = {
        "hw.optional.arm.FEAT_SME",        "hw.optional.arm.FEAT_SME2",
        "hw.optional.arm.FEAT_SME_F64F64", "hw.optional.arm.FEAT_SME_I16I64",
        "hw.optional.arm.FEAT_SME_FA64",   "hw.optional.arm.FEAT_SME2p1",
    };
    for (std::size_t i = 0; i < std::size(features); ++i)
        if (sysctl_flag(features[i]))
            host.features |= 1U << i;
    if (host.sme) {
        int svl = 0;
        std::size_t size = sizeof(svl);
        if (sysctlbyname("hw.optional.arm.sme_max_svl_b", &svl, &size, nullptr,
                         0) != 0 ||
            svl < 16 || svl > static_cast<int>(max_svl) || (svl & (svl - 1)))
            fail("cannot read the streaming vector length");
        host.svl = static_cast<std::uint32_t>(svl);
    }
    return host;
}

// The handler thread's view of the run in progress.
enum class Stage { ready, run, store, done };

struct Run {
    Context context{};
    Bank load{};
    Bank store{};
    State loaded;
    State stored;
    const Case *current = nullptr;
    std::uint32_t *code = nullptr;
    std::atomic<Stage> stage{Stage::done};
    // Results
    std::uint32_t event = fatal;
    std::uint32_t pc_offset = 0;
    std::uint64_t far = 0;
    std::uint32_t esr = 0;
    std::array<std::uint64_t, 32> x{};
    std::uint32_t nzcv = 0;
    std::uint32_t fpsr = 0;  // the epilogue's read
};

Run run;
mach_port_t exception_port = MACH_PORT_NULL;

#pragma pack(push, 4)
struct ExceptionRequest {
    mach_msg_header_t head;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t thread;
    mach_msg_port_descriptor_t task;
    NDR_record_t ndr;
    exception_type_t exception;
    mach_msg_type_number_t code_count;
    std::int64_t code[2];
    mach_msg_trailer_t trailer;
};
struct ExceptionReply {
    mach_msg_header_t head;
    NDR_record_t ndr;
    kern_return_t result;
};
#pragma pack(pop)

[[noreturn]] void internal(const char *what, std::uint64_t pc)
{
    std::fprintf(stderr, "sme_native_oracle: %s at pc %#llx\n", what,
                 static_cast<unsigned long long>(pc));
    std::fflush(stderr);
    _exit(4);
}

void handle(const ExceptionRequest &request)
{
    const thread_t thread = request.thread.name;
    arm_thread_state64_t gpr;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, ARM_THREAD_STATE64,
                         reinterpret_cast<thread_state_t>(&gpr),
                         &count) != KERN_SUCCESS)
        internal("thread_get_state", 0);
    arm_exception_state64_t exception;
    count = ARM_EXCEPTION_STATE64_COUNT;
    if (thread_get_state(thread, ARM_EXCEPTION_STATE64,
                         reinterpret_cast<thread_state_t>(&exception),
                         &count) != KERN_SUCCESS)
        internal("thread_get_state", 0);
    const std::uint64_t pc = gpr.__pc;
    const auto code = reinterpret_cast<std::uint64_t>(run.code);
    switch (run.stage.load()) {
    case Stage::ready: {
        if (pc != reinterpret_cast<std::uint64_t>(oracle_ready) ||
            request.exception != EXC_BREAKPOINT)
            internal("exception while loading a case", pc);
        const State &state = run.current->initial;
        for (unsigned i = 0; i < 29; ++i)
            gpr.__x[i] = state.x[i];
        gpr.__fp = state.x[29];
        gpr.__lr = state.x[30];
        gpr.__sp = state.x[31];
        gpr.__cpsr = (gpr.__cpsr & ~0xf0000000U) | (state.nzcv & 0xf0000000U);
        // The prologue sets FPSR from X0 and reloads X0 from its literal.
        gpr.__x[0] = state.fpsr;
        gpr.__pc = code + prologue_offset;
        run.stage = Stage::run;
        break;
    }
    case Stage::run: {
        const bool inside = pc >= code && pc < code + code_page_size;
        run.pc_offset = inside ? static_cast<std::uint32_t>(pc - code)
                               : 0xffffffffU;
        run.far = exception.__far;
        run.esr = exception.__esr;
        if (!inside)
            run.event = fatal;
        else if (request.exception == EXC_BREAKPOINT &&
                 pc == code + epilogue_trap)
            run.event = epilogue;
        else if (request.exception == EXC_BREAKPOINT)
            run.event = run.code[(pc - code) / 4] == sentinel ? system_call
                                                               : breakpoint;
        else if (request.exception == EXC_BAD_INSTRUCTION &&
                 run.esr >> 26 == 0x2c)
            run.event = floating_point_trap;  // XNU's trapped FP exception
        else if (request.exception == EXC_BAD_INSTRUCTION)
            run.event = illegal_instruction;
        else if (request.exception == EXC_BAD_ACCESS)
            run.event = memory_fault;
        else if (request.exception == EXC_ARITHMETIC)
            run.event = floating_point_trap;
        else
            run.event = fatal;
        // An SP alignment fault leaves FAR unknown; the corpus names the SP.
        if (run.event == memory_fault && run.esr >> 26 == 0x26)
            run.far = gpr.__sp;
        if (run.event != memory_fault)
            run.far = 0;
        for (unsigned i = 0; i < 29; ++i)
            run.x[i] = gpr.__x[i];
        run.x[29] = gpr.__fp;
        run.x[30] = gpr.__lr;
        run.x[31] = gpr.__sp;
        run.nzcv = gpr.__cpsr & 0xf0000000U;
        run.fpsr = static_cast<std::uint32_t>(gpr.__x[0]);
        gpr.__x[0] = reinterpret_cast<std::uint64_t>(&run.context);
        gpr.__pc = reinterpret_cast<std::uint64_t>(oracle_store);
        run.stage = Stage::store;
        break;
    }
    case Stage::store:
        if (pc != reinterpret_cast<std::uint64_t>(oracle_stored) ||
            request.exception != EXC_BREAKPOINT)
            internal("exception while storing a case", pc);
        gpr.__x[0] = reinterpret_cast<std::uint64_t>(&run.context);
        gpr.__pc = reinterpret_cast<std::uint64_t>(oracle_restore);
        run.stage = Stage::done;
        break;
    case Stage::done:
        internal("exception outside a case", pc);
    }
    if (thread_set_state(thread, ARM_THREAD_STATE64,
                         reinterpret_cast<thread_state_t>(&gpr),
                         ARM_THREAD_STATE64_COUNT) != KERN_SUCCESS)
        internal("thread_set_state", pc);
}

void *handler(void *)
{
    for (;;) {
        ExceptionRequest request{};
        if (mach_msg(&request.head, MACH_RCV_MSG, 0, sizeof(request),
                     exception_port, MACH_MSG_TIMEOUT_NONE,
                     MACH_PORT_NULL) != KERN_SUCCESS)
            internal("mach_msg receive", 0);
        handle(request);
        mach_port_deallocate(mach_task_self(), request.thread.name);
        mach_port_deallocate(mach_task_self(), request.task.name);
        ExceptionReply reply{};
        reply.head.msgh_bits =
            MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(request.head.msgh_bits), 0);
        reply.head.msgh_remote_port = request.head.msgh_remote_port;
        reply.head.msgh_local_port = MACH_PORT_NULL;
        reply.head.msgh_size = sizeof(reply);
        reply.head.msgh_id = request.head.msgh_id + 100;
        reply.ndr = NDR_record;
        reply.result = KERN_SUCCESS;
        if (mach_msg(&reply.head, MACH_SEND_MSG, sizeof(reply), 0,
                     MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
                     MACH_PORT_NULL) != KERN_SUCCESS)
            internal("mach_msg reply", 0);
    }
}

// A signal reaches the process only when the exception path itself failed.
// Exiting from the handler keeps the host free of crash reports.
void last_resort(int signo, siginfo_t *info, void *)
{
    char text[96];
    const int length = std::snprintf(
        text, sizeof(text), "sme_native_oracle: signal %d at %p\n", signo,
        info != nullptr ? info->si_addr : nullptr);
    if (length > 0)
        (void)write(2, text, static_cast<std::size_t>(length));
    _exit(signo == SIGALRM ? 3 : 2);
}

// A case runs on its own SP, so the handler runs on an alternate stack.
void install_last_resort()
{
    static std::vector<std::uint8_t> alternate(1 << 16);
    stack_t stack{};
    stack.ss_sp = alternate.data();
    stack.ss_size = alternate.size();
    if (sigaltstack(&stack, nullptr) != 0)
        fail("cannot install the signal stack");
    struct sigaction action {};
    action.sa_sigaction = last_resort;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int signo : {SIGILL, SIGSEGV, SIGBUS, SIGTRAP, SIGFPE, SIGABRT, SIGALRM})
        sigaction(signo, &action, nullptr);
}

std::vector<std::uint8_t> read_file(const char *path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        fail(std::string("cannot read ") + path);
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

Case read_case(Reader &reader, std::uint32_t svl)
{
    Case result;
    const auto word_count = reader.get<std::uint32_t>();
    result.entry = reader.get<std::uint32_t>();
    if (word_count == 0 || word_count > code_page_size / 4 ||
        result.entry % 4 != 0 || result.entry / 4 >= word_count)
        fail("bad program");
    result.words.resize(word_count);
    for (auto &word : result.words)
        word = reader.get<std::uint32_t>();
    result.initial.base(svl);
    read_state(reader, result.initial, svl);
    // Darwin clears X18 whenever a thread returns to user mode, so a case
    // can only start with it zero.
    if (result.initial.x[18] != 0)
        fail("a case's initial X18 is not zero");
    const auto region_count = reader.get<std::uint8_t>();
    for (unsigned i = 0; i < region_count; ++i) {
        Region region;
        region.address = reader.get<std::uint64_t>();
        region.size = reader.get<std::uint32_t>();
        region.flags = reader.get<std::uint8_t>();
        if (region.address % host_page_size || region.size == 0 ||
            region.size % host_page_size || region.address >> 47 ||
            region.flags & ~(region_readable | region_writable | region_explicit))
            fail("bad region");
        region.initial.resize(region.size);
        if (region.flags & region_explicit) {
            reader.copy(region.initial.data(), region.size);
        } else {
            const auto seed = reader.get<std::uint32_t>();
            for (std::uint32_t b = 0; b < region.size; ++b)
                region.initial[b] = region_fill(seed, b);
        }
        result.regions.push_back(std::move(region));
    }
    return result;
}

void bind(Bank &bank, State &state)
{
    bank.svcr = state.svcr;
    bank.fpcr = state.fpcr;
    bank.fpsr = state.fpsr;
    bank.tpidr2 = state.tpidr2;
    bank.v = state.v.data();
    bank.z = state.z.data();
    bank.p = state.p.data();
    bank.za = state.za.data();
    bank.zt0 = state.zt0.data();
}

std::uint32_t branch_to(std::uint32_t from, std::uint32_t to)
{
    const auto distance =
        (static_cast<std::int32_t>(to) - static_cast<std::int32_t>(from)) / 4;
    return branch | (static_cast<std::uint32_t>(distance) & 0x03ffffffU);
}

// What one run of a case left.
struct Outcome {
    std::uint32_t event = fatal;
    std::uint32_t pc_offset = 0;
    std::uint64_t far = 0;
    std::uint32_t esr = 0;
    std::uint32_t fpsr = 0;  // the epilogue's read
    State state;
    std::vector<std::vector<std::uint8_t>> regions;
};

// Runs a case once. With `rerun`, the word at that offset branches to the
// epilogue.
Outcome attempt(const Case &item, const Host &host, std::uint32_t svl,
                const std::uint32_t *rerun)
{
    // The template's buffers are sized for the host's SVL even when the
    // request has none, since a case may still enter streaming mode.
    const std::uint32_t buffer_svl = host.sme ? host.svl : 0;
    run.loaded = item.initial;
    if (svl != buffer_svl) {
        State sized;
        sized.base(buffer_svl);
        sized.x = item.initial.x;
        sized.nzcv = item.initial.nzcv;
        sized.fpcr = item.initial.fpcr;
        sized.fpsr = item.initial.fpsr;
        sized.v = item.initial.v;
        run.loaded = sized;
    }
    run.stored = run.loaded;
    bind(run.load, run.loaded);
    bind(run.store, run.stored);
    run.context.sme = host.sme;
    run.context.sme2 = host.sme2;
    run.context.svl = buffer_svl;
    run.context.load = &run.load;
    run.context.store = &run.store;

    pthread_jit_write_protect_np(0);
    for (std::size_t i = 0; i < code_page_size / 4; ++i)
        run.code[i] = fill_word;
    for (std::size_t i = 0; i < item.words.size(); ++i)
        run.code[i] = item.words[i] == supervisor_call ? sentinel : item.words[i];
    if (rerun != nullptr)
        run.code[*rerun / 4] = branch_to(*rerun, epilogue_offset);
    run.code[prologue_offset / 4] = write_fpsr;
    run.code[prologue_offset / 4 + 1] =
        load_literal | (literal_offset - prologue_offset - 4) / 4 << 5;
    run.code[prologue_offset / 4 + 2] =
        branch_to(prologue_offset + 8, item.entry);
    run.code[epilogue_offset / 4] = read_fpsr;
    run.code[epilogue_trap / 4] = epilogue_break;
    std::memcpy(&run.code[literal_offset / 4], &item.initial.x[0], 8);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(run.code, code_page_size);

    for (const Region &region : item.regions) {
        mach_vm_address_t address = region.address;
        if (mach_vm_allocate(mach_task_self(), &address, region.size,
                             VM_FLAGS_FIXED) != KERN_SUCCESS)
            fail("cannot map a region at its address");
        std::memcpy(reinterpret_cast<void *>(address), region.initial.data(),
                    region.size);
        vm_prot_t protection = VM_PROT_NONE;
        if (region.flags & region_readable)
            protection |= VM_PROT_READ;
        if (region.flags & region_writable)
            protection |= VM_PROT_WRITE;
        if (mach_vm_protect(mach_task_self(), address, region.size, false,
                            protection) != KERN_SUCCESS)
            fail("cannot protect a region");
    }

    run.current = &item;
    run.event = fatal;
    run.stage = Stage::ready;
    alarm(10);
    oracle_enter(&run.context);
    alarm(0);
    if (run.stage.load() != Stage::done)
        fail("the case returned without finishing");

    Outcome outcome;
    outcome.event = run.event;
    outcome.pc_offset = run.pc_offset;
    outcome.far = run.far;
    outcome.esr = run.esr;
    outcome.fpsr = run.fpsr;
    outcome.state = run.stored;
    outcome.state.x = run.x;
    outcome.state.nzcv = run.nzcv;
    outcome.state.svcr = static_cast<std::uint8_t>(run.store.svcr);
    outcome.state.fpcr = static_cast<std::uint32_t>(run.store.fpcr);
    outcome.state.fpsr = static_cast<std::uint32_t>(run.store.fpsr);
    outcome.state.tpidr2 = run.store.tpidr2;
    for (const Region &region : item.regions) {
        const auto address = static_cast<mach_vm_address_t>(region.address);
        if (mach_vm_protect(mach_task_self(), address, region.size, false,
                            VM_PROT_READ) != KERN_SUCCESS)
            fail("cannot read back a region");
        const auto *bytes = reinterpret_cast<const std::uint8_t *>(address);
        outcome.regions.emplace_back(bytes, bytes + region.size);
        mach_vm_deallocate(mach_task_self(), address, region.size);
    }
    return outcome;
}

// Whether a rerun left the bytes of the first run, where a 4-byte chunk may
// differ only by a literal load of the replaced word.
bool same_bytes(const std::vector<std::uint8_t> &first,
                const std::vector<std::uint8_t> &again, std::uint32_t replaced,
                std::uint32_t substitute)
{
    if (first.size() != again.size())
        return false;
    for (std::size_t offset = 0; offset < first.size(); offset += 4) {
        const std::size_t length =
            std::min<std::size_t>(4, first.size() - offset);
        if (std::memcmp(&first[offset], &again[offset], length) == 0)
            continue;
        std::uint32_t a = 0, b = 0;
        if (length != 4)
            return false;
        std::memcpy(&a, &first[offset], 4);
        std::memcpy(&b, &again[offset], 4);
        if (a != replaced || b != substitute)
            return false;
    }
    return true;
}

void check_rerun(const Outcome &first, const Outcome &again,
                 std::uint32_t replaced, std::uint32_t substitute,
                 std::uint32_t index)
{
    const auto bytes = [](const auto &value) {
        std::vector<std::uint8_t> result(sizeof(value));
        std::memcpy(result.data(), &value, sizeof(value));
        return result;
    };
    const auto same = [&](const std::vector<std::uint8_t> &a,
                          const std::vector<std::uint8_t> &b) {
        return same_bytes(a, b, replaced, substitute);
    };
    const State &a = first.state;
    const State &b = again.state;
    bool equal = a.nzcv == b.nzcv && a.fpcr == b.fpcr && a.svcr == b.svcr &&
                 a.tpidr2 == b.tpidr2 && same(a.v, b.v) && same(a.z, b.z) &&
                 same(a.p, b.p) && same(a.za, b.za) && same(a.zt0, b.zt0) &&
                 first.regions.size() == again.regions.size();
    for (unsigned i = 1; i < 32 && equal; ++i)
        equal = same(bytes(a.x[i]), bytes(b.x[i]));
    for (std::size_t r = 0; r < first.regions.size() && equal; ++r)
        equal = same(first.regions[r], again.regions[r]);
    if (again.event != epilogue || !equal)
        fail("case " + std::to_string(index) +
             ": the run that reads FPSR left a different state");
}

void execute(const Case &item, const Host &host, std::uint32_t svl,
             std::uint32_t index, Writer &writer)
{
    if (item.words.size() * 4 > stub_offset)
        fail("a program reaches the oracle's stubs");
    Outcome outcome = attempt(item, host, svl, nullptr);
    // A run that ended in streaming mode reports the FPSR the epilogue
    // reads where it ended.
    if (outcome.event != fatal && (outcome.state.svcr & 1U) != 0U &&
        outcome.pc_offset < stub_offset) {
        const std::uint32_t at = outcome.pc_offset;
        const std::uint32_t slot = at / 4;
        const std::uint32_t replaced =
            slot < item.words.size()
                ? (item.words[slot] == supervisor_call ? sentinel
                                                       : item.words[slot])
                : fill_word;
        for (unsigned attempt_index = 0; attempt_index < fpsr_reruns;
             ++attempt_index) {
            const Outcome again = attempt(item, host, svl, &at);
            check_rerun(outcome, again, replaced,
                        branch_to(at, epilogue_offset), index);
            outcome.state.fpsr = again.fpsr;
            if (again.fpsr != streaming_reset_fpsr)
                break;
        }
    }

    State &final_state = outcome.state;
    if (svl == 0 && (final_state.svcr != 0 || final_state.tpidr2 != 0)) {
        // A case without SME state that leaves streaming mode or ZA on
        // cannot be described; report it as fatal.
        outcome.event = fatal;
        final_state.svcr = 0;
        final_state.tpidr2 = 0;
    }

    writer.put(outcome.event);
    writer.put(outcome.pc_offset);
    writer.put(outcome.far);
    writer.put(outcome.esr);
    write_state(writer, final_state, run.loaded, svl);
    for (std::size_t r = 0; r < item.regions.size(); ++r) {
        const Region &region = item.regions[r];
        const auto &bytes = outcome.regions[r];
        std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
        for (std::uint32_t b = 0; b < region.size;) {
            if (bytes[b] == region.initial[b]) {
                ++b;
                continue;
            }
            std::uint32_t end = b;
            while (end < region.size && bytes[end] != region.initial[end])
                ++end;
            runs.emplace_back(b, end - b);
            b = end;
        }
        writer.put(static_cast<std::uint32_t>(runs.size()));
        for (const auto &[offset, length] : runs) {
            writer.put(offset);
            writer.put(length);
            writer.bytes(bytes.data() + offset, length);
        }
    }
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s REQUEST RESPONSE\n", argv[0]);
        return 64;
    }
    install_last_resort();
    const Host host = probe_host();
    Reader reader(read_file(argv[1]));
    if (std::memcmp(reader.bytes(8), "UMLSMEQ", 8) != 0 ||
        reader.get<std::uint32_t>() != request_version)
        fail("not a version 1 request");
    const auto svl = reader.get<std::uint32_t>();
    const auto count = reader.get<std::uint32_t>();
    if (svl != 0 && svl != host.svl)
        fail("the request's SVL is not this host's");

    void *page = mmap(nullptr, code_page_size,
                      PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (page == MAP_FAILED)
        fail("cannot map the code page");
    run.code = static_cast<std::uint32_t *>(page);

    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                           &exception_port) != KERN_SUCCESS ||
        mach_port_insert_right(mach_task_self(), exception_port, exception_port,
                               MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS)
        fail("cannot create the exception port");
    pthread_t thread;
    if (pthread_create(&thread, nullptr, handler, nullptr) != 0)
        fail("cannot start the handler");
    // The handler thread takes the watchdog's SIGALRM, never a case.
    sigset_t asynchronous;
    sigemptyset(&asynchronous);
    sigaddset(&asynchronous, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &asynchronous, nullptr);
    // Only this thread's traps reach the handler.
    if (thread_set_exception_ports(
            mach_thread_self(),
            EXC_MASK_BAD_INSTRUCTION | EXC_MASK_BAD_ACCESS |
                EXC_MASK_BREAKPOINT | EXC_MASK_ARITHMETIC,
            exception_port,
            static_cast<exception_behavior_t>(EXCEPTION_DEFAULT |
                                              MACH_EXCEPTION_CODES),
            THREAD_STATE_NONE) != KERN_SUCCESS)
        fail("cannot set the exception ports");

    Writer writer;
    writer.bytes(reinterpret_cast<const std::uint8_t *>("UMLSMER"), 8);
    writer.put(response_version);
    writer.put(host.sme ? host.svl : 0U);
    writer.put(host.features);
    writer.put(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const Case item = read_case(reader, svl);
        execute(item, host, svl, i, writer);
    }
    if (!reader.done())
        fail("trailing bytes in the request");
    std::ofstream stream(argv[2], std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(writer.data().data()),
                 static_cast<std::streamsize>(writer.data().size()));
    if (!stream)
        fail(std::string("cannot write ") + argv[2]);
    return 0;
}
