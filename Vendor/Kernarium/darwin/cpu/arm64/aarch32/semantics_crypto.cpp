// SPDX-License-Identifier: GPL-2.0-or-later
//
// The semantic library's Armv8 cryptographic extensions: AESE, AESD, AESMC,
// AESIMC and the SHA-1 and SHA-256 instructions. The A32 and A64 forms
// compute the same functions on the same 128-bit operands, so each runs as
// the host's own instruction on copies of the registers.

#include "cpu/arm64/aarch32/semantics_vector.hpp"

#include <array>
#include <cstddef>
#include <cstring>

namespace uml::cpu::arm64::aarch32::detail {

namespace {

using Block = std::array<std::byte, 16>;

[[nodiscard]] Block block_of(const Exec &x, unsigned d) noexcept
{
	Block block;
	std::memcpy(block.data(), x.cpu.vectors[d >> 1].bytes.data(), 16);
	return block;
}

void set_block(Exec &x, unsigned d, const Block &block) noexcept
{
	std::memcpy(x.cpu.vectors[d >> 1].bytes.data(), block.data(), 16);
}

/* A destination block updated from a source block: `insn` runs on v0 (the
 * destination) and v1. */
#define UML_A32_CRYPTO_BINARY(name, insn)                                              \
	void name(Block &d, const Block &m) noexcept                                   \
	{                                                                              \
		__asm__ volatile("ldr q0, [%0]\n\tldr q1, [%1]\n\t" insn "\n\tstr q0, [%0]" \
				 : : "r"(d.data()), "r"(m.data()) : "v0", "v1", "memory");   \
	}

/* The same with a third block on v2. */
#define UML_A32_CRYPTO_TERNARY(name, insn)                                             \
	void name(Block &d, const Block &n, const Block &m) noexcept                   \
	{                                                                              \
		__asm__ volatile("ldr q0, [%0]\n\tldr q1, [%1]\n\tldr q2, [%2]\n\t" insn  \
				 "\n\tstr q0, [%0]"                                           \
				 : : "r"(d.data()), "r"(n.data()), "r"(m.data())             \
				 : "v0", "v1", "v2", "memory");                              \
	}

UML_A32_CRYPTO_BINARY(host_aese, "aese v0.16b, v1.16b")
UML_A32_CRYPTO_BINARY(host_aesd, "aesd v0.16b, v1.16b")
UML_A32_CRYPTO_BINARY(host_aesmc, "mov v0.16b, v1.16b\n\taesmc v0.16b, v0.16b")
UML_A32_CRYPTO_BINARY(host_aesimc, "mov v0.16b, v1.16b\n\taesimc v0.16b, v0.16b")
UML_A32_CRYPTO_BINARY(host_sha1su1, "sha1su1 v0.4s, v1.4s")
UML_A32_CRYPTO_BINARY(host_sha256su0, "sha256su0 v0.4s, v1.4s")
UML_A32_CRYPTO_TERNARY(host_sha1su0, "sha1su0 v0.4s, v1.4s, v2.4s")
UML_A32_CRYPTO_TERNARY(host_sha256su1, "sha256su1 v0.4s, v1.4s, v2.4s")
UML_A32_CRYPTO_TERNARY(host_sha256h, "sha256h q0, q1, v2.4s")
UML_A32_CRYPTO_TERNARY(host_sha256h2, "sha256h2 q0, q1, v2.4s")
UML_A32_CRYPTO_TERNARY(host_sha1c, "sha1c q0, s1, v2.4s")
UML_A32_CRYPTO_TERNARY(host_sha1p, "sha1p q0, s1, v2.4s")
UML_A32_CRYPTO_TERNARY(host_sha1m, "sha1m q0, s1, v2.4s")

#undef UML_A32_CRYPTO_BINARY
#undef UML_A32_CRYPTO_TERNARY

/* AESE and AESD: Qd = op(Qd xor Qm); the host's instruction does the xor. */
template<void (*Host)(Block &, const Block &)>
bool aes_round(Exec &x)
{
	const unsigned d = static_cast<unsigned>(x.o.d);
	Block destination = block_of(x, d);
	Host(destination, block_of(x, static_cast<unsigned>(x.o.m)));
	set_block(x, d, destination);
	return true;
}

/* AESMC, AESIMC and the one-source SHA instructions that read Qd. */
template<void (*Host)(Block &, const Block &)>
bool two_register(Exec &x)
{
	const unsigned d = static_cast<unsigned>(x.o.d);
	Block destination = block_of(x, d);
	Host(destination, block_of(x, static_cast<unsigned>(x.o.m)));
	set_block(x, d, destination);
	return true;
}

template<void (*Host)(Block &, const Block &, const Block &)>
bool three_register(Exec &x)
{
	const unsigned d = static_cast<unsigned>(x.o.d);
	Block destination = block_of(x, d);
	Host(destination, block_of(x, static_cast<unsigned>(x.o.n)), block_of(x, static_cast<unsigned>(x.o.m)));
	set_block(x, d, destination);
	return true;
}

/* SHA1H: the rotated low word of Qm, zero-extended. */
bool sha1h(Exec &x)
{
	const auto word = static_cast<std::uint32_t>(
		lane128(q_reg(x, static_cast<unsigned>(x.o.m)), 0, 32));
	set_q_reg(x, static_cast<unsigned>(x.o.d), (word << 30 | word >> 2));
	return true;
}

constexpr VectorEntry entries[] = {
	{"AESE", aes_round<host_aese>}, {"AESD", aes_round<host_aesd>},
	{"AESMC", two_register<host_aesmc>}, {"AESIMC", two_register<host_aesimc>},
	{"SHA1C", three_register<host_sha1c>}, {"SHA1P", three_register<host_sha1p>},
	{"SHA1M", three_register<host_sha1m>}, {"SHA1H", sha1h},
	{"SHA1SU0", three_register<host_sha1su0>}, {"SHA1SU1", two_register<host_sha1su1>},
	{"SHA256H", three_register<host_sha256h>}, {"SHA256H2", three_register<host_sha256h2>},
	{"SHA256SU0", two_register<host_sha256su0>}, {"SHA256SU1", three_register<host_sha256su1>},
};

} // namespace

std::span<const VectorEntry> crypto_entries() noexcept
{
	return entries;
}

} // namespace uml::cpu::arm64::aarch32::detail
