// SPDX-License-Identifier: GPL-2.0-or-later
/* What the guest is told its CPU implements, pinned value for value:
 * AT_HWCAP, AT_HWCAP2, every emulated identification register, and the
 * Features line the guest kernel prints in /proc/cpuinfo from the two words.
 * The profiles are named hosts, every host predicate the advertised set reads
 * toggled on and off, FEAT_AFP and FEAT_RPRES with and without the host
 * features AFP needs, five pairs of features no host can have apart, the
 * SME profiles at every streaming vector length, whole and in part, and the
 * M4 as the prober reports it and as the engines present it. No profile
 * advertises SVE: the virtual CPU has none.
 * An intended change to the advertised set changes rows of this table:
 * regenerate it with --print and review the rows that moved. */
#include "cpu/arm64/gadgets/sme_support.hpp"
#include "cpu/arm64/id_registers.hpp"
#include "cpu/arm64/jit/jit_sme_support.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "platform/apple/host_features.hpp"
#include "require.hpp"
#include "uml_darwin_cpu.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace {

using ::uml::platform::ArmFeature;
using ::uml::platform::HostCapabilities;
using ::uml::testing::fail;
using ::uml::testing::require;
namespace detail = ::uml::cpu::arm64::detail;

/* A host predicate the advertised set reads. Eight features are mirrored in
 * a HostCapabilities flag as well, and the derivation reads that flag
 * (implements_afp() reads FEAT_FP16 as the bit), so those two can be toggled
 * apart. */
struct Feature final {
	ArmFeature bit;
	bool HostCapabilities::*flag;
};

constexpr std::array features{
	Feature{ArmFeature::aes, &HostCapabilities::aes},
	Feature{ArmFeature::pmull, &HostCapabilities::pmull},
	Feature{ArmFeature::sha1, &HostCapabilities::sha1},
	Feature{ArmFeature::sha256, &HostCapabilities::sha2},
	Feature{ArmFeature::sha512, &HostCapabilities::sha512},
	Feature{ArmFeature::sha3, &HostCapabilities::sha3},
	Feature{ArmFeature::crc32, &HostCapabilities::crc32},
	Feature{ArmFeature::fp16, &HostCapabilities::fp16},
	Feature{ArmFeature::lse, nullptr},
	Feature{ArmFeature::lse2, nullptr},
	Feature{ArmFeature::rdm, nullptr},
	Feature{ArmFeature::dotprod, nullptr},
	Feature{ArmFeature::fhm, nullptr},
	Feature{ArmFeature::jscvt, nullptr},
	Feature{ArmFeature::fcma, nullptr},
	Feature{ArmFeature::lrcpc, nullptr},
	Feature{ArmFeature::lrcpc2, nullptr},
	Feature{ArmFeature::flag_m, nullptr},
	Feature{ArmFeature::flag_m2, nullptr},
	Feature{ArmFeature::sb, nullptr},
	Feature{ArmFeature::dit, nullptr},
	Feature{ArmFeature::i8mm, nullptr},
	Feature{ArmFeature::frintts, nullptr},
	Feature{ArmFeature::bf16, nullptr},
	Feature{ArmFeature::ecv, nullptr},
	Feature{ArmFeature::afp, nullptr},
	Feature{ArmFeature::rpres, nullptr},
};

/* The second feature of each pair is the first's identification field at
 * its next value (ID_AA64ISAR0_EL1.AES, SHA2 and TS, ID_AA64ISAR1_EL1.LRCPC),
 * so no host implements it without the first. Toggling one keeps the pair
 * possible. */
constexpr std::array<std::pair<ArmFeature, ArmFeature>, 4> extensions{{
	{ArmFeature::aes, ArmFeature::pmull},
	{ArmFeature::sha256, ArmFeature::sha512},
	{ArmFeature::flag_m, ArmFeature::flag_m2},
	{ArmFeature::lrcpc, ArmFeature::lrcpc2},
}};

constexpr std::array afp_needs{ArmFeature::fp16, ArmFeature::fhm,
	ArmFeature::bf16, ArmFeature::frintts, ArmFeature::fcma,
	ArmFeature::jscvt};

enum class Part { both, bit, flag };

[[nodiscard]] const Feature &feature(ArmFeature bit)
{
	for (const auto &candidate : features)
		if (candidate.bit == bit)
			return candidate;
	fail("not a feature the advertised set reads");
}

[[nodiscard]] std::string name(ArmFeature bit)
{
	return std::string{::uml::platform::arm_feature_tokens[
		static_cast<std::size_t>(bit)]};
}

void assign(HostCapabilities &host, ArmFeature bit, bool on,
	Part part = Part::both)
{
	const auto &target = feature(bit);
	if (part != Part::flag)
		host.supported.set(static_cast<std::size_t>(bit), on);
	if (part != Part::bit && target.flag != nullptr)
		host.*target.flag = on;
}

[[nodiscard]] HostCapabilities with(std::initializer_list<ArmFeature> bits)
{
	HostCapabilities host;
	for (const auto bit : bits)
		assign(host, bit, true);
	return host;
}

/* Turning a feature on turns on the one it extends; turning it off turns off
 * the one that extends it. */
[[nodiscard]] HostCapabilities toggled(HostCapabilities host, ArmFeature bit,
	Part part)
{
	const bool on = part == Part::flag ? !(host.*feature(bit).flag) :
		!host.has(bit);
	assign(host, bit, on, part);
	for (const auto &[base, extension] : extensions) {
		if (on && bit == extension)
			assign(host, base, true, part);
		if (!on && bit == base)
			assign(host, extension, false, part);
	}
	return host;
}

struct Profile final {
	std::string name;
	HostCapabilities host;
};

/* `host` with the SME profile's tokens, SME2 or not, and a streaming vector
 * length, as the prober reports them with the guest-SME policy on. */
[[nodiscard]] HostCapabilities with_sme(HostCapabilities host, unsigned svl,
	bool sme2)
{
	for (const auto bit : {ArmFeature::sme, ArmFeature::sme_f64f64,
		     ArmFeature::sme_i16i64})
		host.supported.set(static_cast<std::size_t>(bit));
	host.supported.set(static_cast<std::size_t>(ArmFeature::sme2), sme2);
	host.sme_svl_bytes = static_cast<std::uint16_t>(svl);
	return host;
}

[[nodiscard]] std::vector<Profile> profiles()
{
	const HostCapabilities none;
	HostCapabilities all;
	for (const auto &each : features)
		assign(all, each.bit, true);
	/* The features an M1 Ultra's sysctls report. */
	const auto m1 = with({ArmFeature::aes, ArmFeature::pmull,
		ArmFeature::sha1, ArmFeature::sha256, ArmFeature::sha512,
		ArmFeature::sha3, ArmFeature::crc32, ArmFeature::fp16,
		ArmFeature::lse, ArmFeature::lse2, ArmFeature::rdm,
		ArmFeature::dotprod, ArmFeature::fhm, ArmFeature::jscvt,
		ArmFeature::fcma, ArmFeature::lrcpc, ArmFeature::lrcpc2,
		ArmFeature::flag_m, ArmFeature::flag_m2, ArmFeature::sb,
		ArmFeature::dit, ArmFeature::frintts});

	std::vector<Profile> result{{"none", none},
		{"baseline", ::uml::cpu::baseline_host_capabilities()},
		{"m1-ultra", m1}, {"all", all}};
	const std::array<std::pair<const char *, HostCapabilities>, 3> bases{{
		{"none", none}, {"m1-ultra", m1}, {"all", all}}};
	for (const auto &[base_name, base] : bases)
		for (const auto &each : features)
			result.push_back({std::string{base_name} + " ^" +
					name(each.bit),
				toggled(base, each.bit, Part::both)});
	for (const auto &each : features) {
		if (each.flag == nullptr)
			continue;
		result.push_back({"m1-ultra ^" + name(each.bit) + " flag",
			toggled(m1, each.bit, Part::flag)});
		result.push_back({"m1-ultra ^" + name(each.bit) + " bit",
			toggled(m1, each.bit, Part::bit)});
	}

	/* FEAT_AFP and FEAT_RPRES on a host with every other feature, then
	 * with each feature AFP needs missing in turn. */
	auto afp_base = all;
	assign(afp_base, ArmFeature::afp, false);
	assign(afp_base, ArmFeature::rpres, false);
	for (std::size_t missing = 0; missing <= afp_needs.size(); ++missing)
		for (const bool afp : {false, true})
			for (const bool rpres : {false, true}) {
				auto host = afp_base;
				std::string label = std::string{"afp="} +
					(afp ? "1" : "0") + " rpres=" +
					(rpres ? "1" : "0");
				if (missing < afp_needs.size()) {
					assign(host, afp_needs[missing], false);
					label += " -" + name(afp_needs[missing]);
				}
				assign(host, ArmFeature::afp, afp);
				assign(host, ArmFeature::rpres, rpres);
				result.push_back({label, host});
			}
	result.push_back({"none +afp", with({ArmFeature::afp})});
	result.push_back({"none +rpres", with({ArmFeature::rpres})});
	result.push_back({"none +afp +rpres",
		with({ArmFeature::afp, ArmFeature::rpres})});

	/* The pairs apart, which no host reports. AT_HWCAP follows the
	 * field, as on Linux: none of PMULL, FLAGM2 or ILRCPC without the
	 * lower value, and SHA2 with SHA512, whose field reads 2. */
	for (const auto &[base, extension] : extensions) {
		result.push_back({"none +" + name(extension) + " alone",
			with({extension})});
		auto host = all;
		assign(host, base, false);
		result.push_back({"all -" + name(base) + " alone", host});
	}

	/* The SME profile, with and without SME2, at every streaming vector
	 * length; the words never depend on which. Then hosts that lack a part
	 * of it or report a length no implementation has, which get none. */
	for (const unsigned svl : {16U, 32U, 64U, 128U, 256U}) {
		result.push_back({"m1-ultra +sme2 svl=" + std::to_string(svl),
			with_sme(m1, svl, true)});
		result.push_back({"m1-ultra +sme svl=" + std::to_string(svl),
			with_sme(m1, svl, false)});
	}
	result.push_back({"none +sme2 svl=64", with_sme(none, 64, true)});
	result.push_back({"all +sme2 svl=64", with_sme(all, 64, true)});
	result.push_back({"m1-ultra +sme2 svl=48", with_sme(m1, 48, true)});
	result.push_back({"m1-ultra +sme2 svl=512", with_sme(m1, 512, true)});
	for (const auto missing : {ArmFeature::sme, ArmFeature::sme_f64f64,
		     ArmFeature::sme_i16i64}) {
		auto host = with_sme(m1, 64, true);
		host.supported.reset(static_cast<std::size_t>(missing));
		result.push_back({"m1-ultra +sme2 svl=64 -" + name(missing), host});
	}

	/* The iPad's A15: the M1 Ultra's features plus FEAT_I8MM, FEAT_BF16
	 * and FEAT_ECV (docs/testing.md). */
	auto a15 = m1;
	for (const auto feature : {ArmFeature::i8mm, ArmFeature::bf16,
		     ArmFeature::ecv})
		assign(a15, feature, true);
	result.push_back({"a15", a15});

	/* The M4: every feature above but CSSC, which every host is given
	 * anyway, as the prober reports it with the guest-SME policy off, then
	 * with SME and SME2 at SVL 64, then as both engines present that host,
	 * which is what its guest sees with the SME policy on. */
	const auto m4 = with({ArmFeature::aes, ArmFeature::pmull,
		ArmFeature::sha1, ArmFeature::sha256, ArmFeature::sha512,
		ArmFeature::sha3, ArmFeature::crc32, ArmFeature::fp16,
		ArmFeature::lse, ArmFeature::lse2, ArmFeature::rdm,
		ArmFeature::dotprod, ArmFeature::fhm, ArmFeature::jscvt,
		ArmFeature::fcma, ArmFeature::lrcpc, ArmFeature::lrcpc2,
		ArmFeature::flag_m, ArmFeature::flag_m2, ArmFeature::sb,
		ArmFeature::dit, ArmFeature::i8mm, ArmFeature::frintts,
		ArmFeature::bf16, ArmFeature::ecv, ArmFeature::afp,
		ArmFeature::rpres});
	namespace arm64 = ::uml::cpu::arm64;
	result.push_back({"m4", m4});
	result.push_back({"m4 +sme2 svl=64", with_sme(m4, 64, true)});
	result.push_back({"m4 +sme2 svl=64 engines",
		arm64::sme_host_for_support(with_sme(m4, 64, true),
			std::min(arm64::gadgets::sme_support,
				arm64::jit::sme_support))});
	return result;
}

/* ID_AA64PFR0_EL1, ID_AA64PFR1_EL1, ID_AA64ZFR0_EL1, ID_AA64SMFR0_EL1,
 * ID_AA64ISAR0_EL1, ID_AA64ISAR1_EL1, ID_AA64ISAR2_EL1, ID_AA64MMFR0_EL1,
 * ID_AA64MMFR1_EL1 and ID_AA64MMFR2_EL1, read into X0: the registers whose
 * fields carry features. */
constexpr std::array<std::uint32_t, 10> feature_registers{0xd538'0400U,
	0xd538'0420U, 0xd538'0480U, 0xd538'04a0U, 0xd538'0600U, 0xd538'0620U,
	0xd538'0640U, 0xd538'0700U, 0xd538'0720U, 0xd538'0740U};

struct Advertised final {
	const char *profile;
	unsigned long hwcap;
	unsigned long hwcap2;
	std::array<std::uint64_t, feature_registers.size()> registers;
};

/* Generated by --print. */
constexpr Advertised pinned[]{
	{"none", 0x807, 0x80400000000,
		{0, 0, 0, 0, 0, 0, 0x10000000010000, 0, 0, 0}},
	{"baseline", 0x807, 0x80400000000,
		{0, 0, 0, 0, 0, 0, 0x10000000010000, 0, 0, 0}},
	{"m1-ultra", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"all", 0x2fb2ffff, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"none ^aes", 0x80f, 0x80400000000,
		{0, 0, 0, 0, 0x10, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^pmull", 0x81f, 0x80400000000,
		{0, 0, 0, 0, 0x20, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^sha1", 0x827, 0x80400000000,
		{0, 0, 0, 0, 0x100, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^sha256", 0x847, 0x80400000000,
		{0, 0, 0, 0, 0x1000, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^sha512", 0x200847, 0x80400000000,
		{0, 0, 0, 0, 0x2000, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^sha3", 0x20807, 0x80400000000,
		{0, 0, 0, 0, 0x100000000, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^crc32", 0x887, 0x80400000000,
		{0, 0, 0, 0, 0x10000, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^fp16", 0xe07, 0x80400000000,
		{0x110000, 0, 0, 0, 0, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^lse", 0x907, 0x80400000000,
		{0, 0, 0, 0, 0x200000, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^lse2", 0x2000807, 0x80400000000,
		{0, 0, 0, 0, 0, 0, 0x10000000010000, 0, 0, 0x100000000}},
	{"none ^rdm", 0x1807, 0x80400000000,
		{0, 0, 0, 0, 0x10000000, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^dotprod", 0x100807, 0x80400000000,
		{0, 0, 0, 0, 0x100000000000, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^fhm", 0x800807, 0x80400000000,
		{0, 0, 0, 0, 0x1000000000000, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^jscvt", 0x2807, 0x80400000000,
		{0, 0, 0, 0, 0, 0x1000, 0x10000000010000, 0, 0, 0}},
	{"none ^fcma", 0x4807, 0x80400000000,
		{0, 0, 0, 0, 0, 0x10000, 0x10000000010000, 0, 0, 0}},
	{"none ^lrcpc", 0x8807, 0x80400000000,
		{0, 0, 0, 0, 0, 0x100000, 0x10000000010000, 0, 0, 0}},
	{"none ^lrcpc2", 0x4008807, 0x80400000000,
		{0, 0, 0, 0, 0, 0x200000, 0x10000000010000, 0, 0, 0}},
	{"none ^flag_m", 0x8000807, 0x80400000000,
		{0, 0, 0, 0, 0x10000000000000, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^flag_m2", 0x8000807, 0x80400000080,
		{0, 0, 0, 0, 0x20000000000000, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^sb", 0x20000807, 0x80400000000,
		{0, 0, 0, 0, 0, 0x1000000000, 0x10000000010000, 0, 0, 0}},
	{"none ^dit", 0x1000807, 0x80400000000,
		{0x1000000000000, 0, 0, 0, 0, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^i8mm", 0x807, 0x80400002000,
		{0, 0, 0, 0, 0, 0x10000000000000, 0x10000000010000, 0, 0, 0}},
	{"none ^frintts", 0x807, 0x80400000100,
		{0, 0, 0, 0, 0, 0x100000000, 0x10000000010000, 0, 0, 0}},
	{"none ^bf16", 0x807, 0x80400004000,
		{0, 0, 0, 0, 0, 0x100000000000, 0x10000000010000, 0, 0, 0}},
	{"none ^ecv", 0x807, 0x80400080000,
		{0, 0, 0, 0, 0, 0, 0x10000000010000, 0x1000000000000000, 0,
			0}},
	{"none ^afp", 0x807, 0x80400000000,
		{0, 0, 0, 0, 0, 0, 0x10000000010000, 0, 0, 0}},
	{"none ^rpres", 0x807, 0x80400000000,
		{0, 0, 0, 0, 0, 0, 0x10000000010000, 0, 0, 0}},
	{"m1-ultra ^aes", 0x2fb2ffe7, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212100, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^pmull", 0x2fb2ffef, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212110, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^sha1", 0x2fb2ffdf, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212020, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^sha256", 0x2f92ffbf, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110210120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^sha512", 0x2f92ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110211120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^sha3", 0x2fb0ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100010212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^crc32", 0x2fb2ff7f, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110202120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^fp16", 0x2fb2f9ff, 0x80400000180,
		{0x1000000000000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^lse", 0x2fb2feff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110012120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^lse2", 0x2db2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0}},
	{"m1-ultra ^rdm", 0x2fb2efff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100100212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^dotprod", 0x2fa2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21000110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^fhm", 0x2f32ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x20100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^jscvt", 0x2fb2dfff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100210000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^fcma", 0x2fb2bfff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100201000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^lrcpc", 0x2bb27fff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100011000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^lrcpc2", 0x2bb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100111000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^flag_m", 0x27b2ffff, 0x80400000100,
		{0x1000000110000, 0, 0, 0, 0x1100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^flag_m2", 0x2fb2ffff, 0x80400000100,
		{0x1000000110000, 0, 0, 0, 0x11100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^sb", 0xfb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^dit", 0x2eb2ffff, 0x80400000180,
		{0x110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^i8mm", 0x2fb2ffff, 0x80400002180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10001100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^frintts", 0x2fb2ffff, 0x80400000080,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1000211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^bf16", 0x2fb2ffff, 0x80400004180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x101100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^ecv", 0x2fb2ffff, 0x80400080180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"m1-ultra ^afp", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^rpres", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"all ^aes", 0x2fb2ffe7, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110212100, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^pmull", 0x2fb2ffef, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110212110, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^sha1", 0x2fb2ffdf, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110212020, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^sha256", 0x2f92ffbf, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110210120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^sha512", 0x2f92ffff, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110211120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^sha3", 0x2fb0ffff, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100010212120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^crc32", 0x2fb2ff7f, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110202120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^fp16", 0x2fb2f9ff, 0x80400086180,
		{0x1000000000000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"all ^lse", 0x2fb2feff, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110012120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^lse2", 0x2db2ffff, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0}},
	{"all ^rdm", 0x2fb2efff, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100100212120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^dotprod", 0x2fa2ffff, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21000110212120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^fhm", 0x2f32ffff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x20100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"all ^jscvt", 0x2fb2dfff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100210000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"all ^fcma", 0x2fb2bfff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100201000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"all ^lrcpc", 0x2bb27fff, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100011000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^lrcpc2", 0x2bb2ffff, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100111000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^flag_m", 0x27b2ffff, 0x80400386100,
		{0x1000000110000, 0, 0, 0, 0x1100110212120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^flag_m2", 0x2fb2ffff, 0x80400386100,
		{0x1000000110000, 0, 0, 0, 0x11100110212120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^sb", 0xfb2ffff, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10100100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^dit", 0x2eb2ffff, 0x80400386180,
		{0x110000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^i8mm", 0x2fb2ffff, 0x80400384180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"all ^frintts", 0x2fb2ffff, 0x80400086080,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101000211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"all ^bf16", 0x2fb2ffff, 0x80400082180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10001100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"all ^ecv", 0x2fb2ffff, 0x80400306180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010010, 0, 0x100000000000, 0x100000000}},
	{"all ^afp", 0x2fb2ffff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"all ^rpres", 0x2fb2ffff, 0x80400186180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"m1-ultra ^aes flag", 0x2fb2ffe7, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212100, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^aes bit", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^pmull flag", 0x2fb2ffef, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212110, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^pmull bit", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^sha1 flag", 0x2fb2ffdf, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212020, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^sha1 bit", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^sha256 flag", 0x2f92ffbf, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110210120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^sha256 bit", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^sha512 flag", 0x2f92ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110211120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^sha512 bit", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^sha3 flag", 0x2fb0ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100010212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^sha3 bit", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^crc32 flag", 0x2fb2ff7f, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110202120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^crc32 bit", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^fp16 flag", 0x2fb2f9ff, 0x80400000180,
		{0x1000000000000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra ^fp16 bit", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"afp=0 rpres=0 -fp16", 0x2fb2f9ff, 0x80400086180,
		{0x1000000000000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=0 rpres=1 -fp16", 0x2fb2f9ff, 0x80400086180,
		{0x1000000000000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=1 rpres=0 -fp16", 0x2fb2f9ff, 0x80400086180,
		{0x1000000000000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=1 rpres=1 -fp16", 0x2fb2f9ff, 0x80400086180,
		{0x1000000000000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=0 rpres=0 -fhm", 0x2f32ffff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x20100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=0 rpres=1 -fhm", 0x2f32ffff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x20100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=1 rpres=0 -fhm", 0x2f32ffff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x20100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=1 rpres=1 -fhm", 0x2f32ffff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x20100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=0 rpres=0 -bf16", 0x2fb2ffff, 0x80400082180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10001100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=0 rpres=1 -bf16", 0x2fb2ffff, 0x80400082180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10001100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=1 rpres=0 -bf16", 0x2fb2ffff, 0x80400082180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10001100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=1 rpres=1 -bf16", 0x2fb2ffff, 0x80400082180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10001100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=0 rpres=0 -frintts", 0x2fb2ffff, 0x80400086080,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101000211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=0 rpres=1 -frintts", 0x2fb2ffff, 0x80400086080,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101000211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=1 rpres=0 -frintts", 0x2fb2ffff, 0x80400086080,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101000211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=1 rpres=1 -frintts", 0x2fb2ffff, 0x80400086080,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101000211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=0 rpres=0 -fcma", 0x2fb2bfff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100201000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=0 rpres=1 -fcma", 0x2fb2bfff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100201000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=1 rpres=0 -fcma", 0x2fb2bfff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100201000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=1 rpres=1 -fcma", 0x2fb2bfff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100201000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=0 rpres=0 -jscvt", 0x2fb2dfff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100210000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=0 rpres=1 -jscvt", 0x2fb2dfff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100210000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=1 rpres=0 -jscvt", 0x2fb2dfff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100210000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=1 rpres=1 -jscvt", 0x2fb2dfff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100210000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=0 rpres=0", 0x2fb2ffff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=0 rpres=1", 0x2fb2ffff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"afp=1 rpres=0", 0x2fb2ffff, 0x80400186180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"afp=1 rpres=1", 0x2fb2ffff, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"none +afp", 0x807, 0x80400000000,
		{0, 0, 0, 0, 0, 0, 0x10000000010000, 0, 0, 0}},
	{"none +rpres", 0x807, 0x80400000000,
		{0, 0, 0, 0, 0, 0, 0x10000000010000, 0, 0, 0}},
	{"none +afp +rpres", 0x807, 0x80400000000,
		{0, 0, 0, 0, 0, 0, 0x10000000010000, 0, 0, 0}},
	{"none +pmull alone", 0x807, 0x80400000000,
		{0, 0, 0, 0, 0, 0, 0x10000000010000, 0, 0, 0}},
	{"all -aes alone", 0x2fb2ffe7, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110212100, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"none +sha512 alone", 0x200847, 0x80400000000,
		{0, 0, 0, 0, 0x2000, 0, 0x10000000010000, 0, 0, 0}},
	{"all -sha256 alone", 0x2fb2ffff, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"none +flag_m2 alone", 0x807, 0x80400000000,
		{0, 0, 0, 0, 0, 0, 0x10000000010000, 0, 0, 0}},
	{"all -flag_m alone", 0x27b2ffff, 0x80400386100,
		{0x1000000110000, 0, 0, 0, 0x1100110212120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"none +lrcpc2 alone", 0x807, 0x80400000000,
		{0, 0, 0, 0, 0, 0, 0x10000000010000, 0, 0, 0}},
	{"all -lrcpc alone", 0x2bb27fff, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100011000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"m1-ultra +sme2 svl=16", 0x2fb2ffff, 0x9a43f800180,
		{0x1000000110000, 0x2000000, 0, 0x1f150ff00000000,
			0x21100110212120, 0x1100211000, 0x10000000010000, 0, 0,
			0x100000000}},
	{"m1-ultra +sme svl=16", 0x2fb2ffff, 0x8043f800180,
		{0x1000000110000, 0x1000000, 0, 0xf100fd00000000,
			0x21100110212120, 0x1100211000, 0x10000000010000, 0, 0,
			0x100000000}},
	{"m1-ultra +sme2 svl=32", 0x2fb2ffff, 0x9a43f800180,
		{0x1000000110000, 0x2000000, 0, 0x1f150ff00000000,
			0x21100110212120, 0x1100211000, 0x10000000010000, 0, 0,
			0x100000000}},
	{"m1-ultra +sme svl=32", 0x2fb2ffff, 0x8043f800180,
		{0x1000000110000, 0x1000000, 0, 0xf100fd00000000,
			0x21100110212120, 0x1100211000, 0x10000000010000, 0, 0,
			0x100000000}},
	{"m1-ultra +sme2 svl=64", 0x2fb2ffff, 0x9a43f800180,
		{0x1000000110000, 0x2000000, 0, 0x1f150ff00000000,
			0x21100110212120, 0x1100211000, 0x10000000010000, 0, 0,
			0x100000000}},
	{"m1-ultra +sme svl=64", 0x2fb2ffff, 0x8043f800180,
		{0x1000000110000, 0x1000000, 0, 0xf100fd00000000,
			0x21100110212120, 0x1100211000, 0x10000000010000, 0, 0,
			0x100000000}},
	{"m1-ultra +sme2 svl=128", 0x2fb2ffff, 0x9a43f800180,
		{0x1000000110000, 0x2000000, 0, 0x1f150ff00000000,
			0x21100110212120, 0x1100211000, 0x10000000010000, 0, 0,
			0x100000000}},
	{"m1-ultra +sme svl=128", 0x2fb2ffff, 0x8043f800180,
		{0x1000000110000, 0x1000000, 0, 0xf100fd00000000,
			0x21100110212120, 0x1100211000, 0x10000000010000, 0, 0,
			0x100000000}},
	{"m1-ultra +sme2 svl=256", 0x2fb2ffff, 0x9a43f800180,
		{0x1000000110000, 0x2000000, 0, 0x1f150ff00000000,
			0x21100110212120, 0x1100211000, 0x10000000010000, 0, 0,
			0x100000000}},
	{"m1-ultra +sme svl=256", 0x2fb2ffff, 0x8043f800180,
		{0x1000000110000, 0x1000000, 0, 0xf100fd00000000,
			0x21100110212120, 0x1100211000, 0x10000000010000, 0, 0,
			0x100000000}},
	{"none +sme2 svl=64", 0x807, 0x9a43f800000,
		{0, 0x2000000, 0, 0x1f150ff00000000, 0, 0, 0x10000000010000, 0,
			0, 0}},
	{"all +sme2 svl=64", 0x2fb2ffff, 0x9a43fb86180,
		{0x1000000110000, 0x2000000, 0, 0x1f150ff00000000,
			0x21100110212120, 0x10101100211000, 0x10000000010010,
			0x1000000000000000, 0x100000000000, 0x100000000}},
	{"m1-ultra +sme2 svl=48", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra +sme2 svl=512", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra +sme2 svl=64 -sme", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra +sme2 svl=64 -sme_f64f64", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"m1-ultra +sme2 svl=64 -sme_i16i64", 0x2fb2ffff, 0x80400000180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x1100211000,
			0x10000000010000, 0, 0, 0x100000000}},
	{"a15", 0x2fb2ffff, 0x80400086180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010000, 0x1000000000000000, 0, 0x100000000}},
	{"m4", 0x2fb2ffff, 0x80400386180,
		{0x1000000110000, 0, 0, 0, 0x21100110212120, 0x10101100211000,
			0x10000000010010, 0x1000000000000000, 0x100000000000,
			0x100000000}},
	{"m4 +sme2 svl=64", 0x2fb2ffff, 0x9a43fb86180,
		{0x1000000110000, 0x2000000, 0, 0x1f150ff00000000,
			0x21100110212120, 0x10101100211000, 0x10000000010010,
			0x1000000000000000, 0x100000000000, 0x100000000}},
	{"m4 +sme2 svl=64 engines", 0x2fb2ffff, 0x8043fb86180,
		{0x1000000110000, 0x1000000, 0, 0xf100fd00000000,
			0x21100110212120, 0x10101100211000, 0x10000000010010,
			0x1000000000000000, 0x100000000000, 0x100000000}},
};

/* The Features line, for the named hosts. */
constexpr std::pair<const char *, const char *> pinned_features[]{
		{"none",
			"fp asimd evtstrm cpuid cssc mops"},
		{"baseline",
			"fp asimd evtstrm cpuid cssc mops"},
		{"m1-ultra",
			"fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics "
			"fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc sha3 "
			"asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb "
			"flagm2 frint cssc mops"},
		{"all",
			"fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics "
			"fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc sha3 "
			"asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb "
			"flagm2 frint i8mm bf16 ecv afp rpres cssc mops"},
		{"m1-ultra +sme svl=16",
			"fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics "
			"fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc sha3 "
			"asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb "
			"flagm2 frint sme smei16i64 smef64f64 smei8i32 "
			"smef16f32 smeb16f32 smef32f32 cssc mops"},
		{"m1-ultra +sme svl=32",
			"fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics "
			"fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc sha3 "
			"asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb "
			"flagm2 frint sme smei16i64 smef64f64 smei8i32 "
			"smef16f32 smeb16f32 smef32f32 cssc mops"},
		{"m1-ultra +sme svl=64",
			"fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics "
			"fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc sha3 "
			"asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb "
			"flagm2 frint sme smei16i64 smef64f64 smei8i32 "
			"smef16f32 smeb16f32 smef32f32 cssc mops"},
		{"m1-ultra +sme svl=128",
			"fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics "
			"fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc sha3 "
			"asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb "
			"flagm2 frint sme smei16i64 smef64f64 smei8i32 "
			"smef16f32 smeb16f32 smef32f32 cssc mops"},
		{"m1-ultra +sme svl=256",
			"fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics "
			"fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc sha3 "
			"asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb "
			"flagm2 frint sme smei16i64 smef64f64 smei8i32 "
			"smef16f32 smeb16f32 smef32f32 cssc mops"},
		{"m1-ultra +sme2 svl=64",
			"fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics "
			"fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc sha3 "
			"asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb "
			"flagm2 frint sme smei16i64 smef64f64 smei8i32 "
			"smef16f32 smeb16f32 smef32f32 cssc sme2 smei16i32 "
			"smebi32i32 mops"},
		{"a15",
			"fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics "
			"fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc sha3 "
			"asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb "
			"flagm2 frint i8mm bf16 ecv cssc mops"},
		{"m4",
			"fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics "
			"fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc sha3 "
			"asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb "
			"flagm2 frint i8mm bf16 ecv afp rpres cssc mops"},
		{"m4 +sme2 svl=64",
			"fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics "
			"fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc sha3 "
			"asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb "
			"flagm2 frint i8mm bf16 ecv afp rpres sme smei16i64 "
			"smef64f64 smei8i32 smef16f32 smeb16f32 smef32f32 cssc "
			"sme2 smei16i32 smebi32i32 mops"},
		{"m4 +sme2 svl=64 engines",
			"fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics "
			"fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc sha3 "
			"asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb "
			"flagm2 frint i8mm bf16 ecv afp rpres sme smei16i64 "
			"smef64f64 smei8i32 smef16f32 smeb16f32 smef32f32 cssc "
			"mops"},
};

[[nodiscard]] Advertised advertised(const Profile &profile)
{
	const auto words = detail::guest_hwcaps(profile.host);
	Advertised result{profile.name.c_str(), words[0], words[1], {}};
	const ::uml::cpu::HostCapabilitiesScope scope{profile.host};
	for (std::size_t index = 0; index < feature_registers.size(); ++index)
		result.registers[index] =
			detail::emulated_id_register_value(
				feature_registers[index]);
	/* The rest of the emulated space: MIDR_EL1 and MPIDR_EL1, then
	 * registers that read as zero on every host. */
	for (std::uint32_t crm = 0; crm < 8U; ++crm)
		for (std::uint32_t op2 = 0; op2 < 8U; ++op2) {
			const std::uint32_t mrs =
				0xd538'0000U | crm << 8U | op2 << 5U;
			if (!detail::emulated_id_register(mrs))
				continue;
			bool carries = false;
			for (const auto feature_register : feature_registers)
				carries = carries || mrs == feature_register;
			if (carries)
				continue;
			const std::uint64_t expected = mrs == 0xd538'0000U ?
				0x000f'0000U : mrs == 0xd538'00a0U ?
				0x8000'0000U : 0U;
			require(detail::emulated_id_register_value(mrs) ==
					expected,
				profile.name + ": an emulated register that "
					"carries no feature changed");
		}
	return result;
}

/* arm64 Linux's name for every bit the guest can be given, from the
 * hwcap_str table of arch/arm64/kernel/cpuinfo.c that the guest kernel
 * prints the Features line from: AT_HWCAP's bits from 0 up as 0-63, then
 * AT_HWCAP2's as 64-127. */
constexpr std::pair<unsigned, const char *> hwcap_names[]{
	{0, "fp"}, {1, "asimd"}, {2, "evtstrm"}, {3, "aes"}, {4, "pmull"},
	{5, "sha1"}, {6, "sha2"}, {7, "crc32"}, {8, "atomics"}, {9, "fphp"},
	{10, "asimdhp"}, {11, "cpuid"}, {12, "asimdrdm"}, {13, "jscvt"},
	{14, "fcma"}, {15, "lrcpc"}, {17, "sha3"}, {20, "asimddp"},
	{21, "sha512"}, {22, "sve"}, {23, "asimdfhm"}, {24, "dit"},
	{25, "uscat"}, {26, "ilrcpc"}, {27, "flagm"}, {29, "sb"},
	{64 + 1, "sve2"}, {64 + 2, "sveaes"}, {64 + 3, "svepmull"},
	{64 + 5, "svesha3"}, {64 + 7, "flagm2"}, {64 + 8, "frint"},
	{64 + 9, "svei8mm"}, {64 + 12, "svebf16"}, {64 + 13, "i8mm"},
	{64 + 14, "bf16"},
	{64 + 19, "ecv"}, {64 + 20, "afp"}, {64 + 21, "rpres"},
	{64 + 23, "sme"}, {64 + 24, "smei16i64"}, {64 + 25, "smef64f64"},
	{64 + 26, "smei8i32"}, {64 + 27, "smef16f32"}, {64 + 28, "smeb16f32"},
	{64 + 29, "smef32f32"}, {64 + 34, "cssc"}, {64 + 37, "sme2"},
	{64 + 39, "smei16i32"}, {64 + 40, "smebi32i32"}, {64 + 43, "mops"},
};

[[nodiscard]] std::string features_line(const Advertised &advertised)
{
	std::array<unsigned long, 2> named{};
	std::string line;
	for (const auto &[bit, label] : hwcap_names) {
		const unsigned long mask = 1UL << (bit % 64U);
		named[bit / 64U] |= mask;
		if (((bit < 64U ? advertised.hwcap : advertised.hwcap2) &
		     mask) == 0)
			continue;
		if (!line.empty())
			line += ' ';
		line += label;
	}
	require((advertised.hwcap & ~named[0]) == 0 &&
			(advertised.hwcap2 & ~named[1]) == 0,
		std::string{advertised.profile} +
			": a bit with no name in hwcap_names is advertised");
	return line;
}

[[nodiscard]] const Profile &find(const std::vector<Profile> &all,
	const char *profile_name)
{
	for (const auto &profile : all)
		if (profile.name == profile_name)
			return profile;
	fail(std::string{profile_name} + ": no such profile");
}

/* Prints the two tables above, wrapped before column 80. */
void print(const std::vector<Profile> &all)
{
	for (const auto &profile : all) {
		const auto observed = advertised(profile);
		std::printf("\t{\"%s\", %#lx, %#lx,\n\t\t{", observed.profile,
			observed.hwcap, observed.hwcap2);
		std::size_t column = 17;
		for (std::size_t index = 0; index < observed.registers.size();
		     ++index) {
			char number[24];
			const auto length = static_cast<std::size_t>(
				std::snprintf(number, sizeof number,
					"%#" PRIx64 "%s",
					observed.registers[index],
					index + 1 == observed.registers.size() ?
						"}}," : ","));
			if (index != 0 && column + 1 + length > 79) {
				std::printf("\n\t\t\t");
				column = 24;
			} else if (index != 0) {
				std::printf(" ");
				++column;
			}
			std::printf("%s", number);
			column += length;
		}
		std::printf("\n");
	}
	for (const auto &[profile_name, pinned_line] : pinned_features) {
		const auto line = features_line(advertised(find(all,
			profile_name)));
		std::printf("\t\t{\"%s\",\n", profile_name);
		std::size_t start = 0;
		do {
			const auto end = line.size() - start <= 54 ?
				line.size() : line.rfind(' ', start + 54) + 1;
			std::printf("\t\t\t\"%s\"%s\n",
				line.substr(start, end - start).c_str(),
				end == line.size() ? "}," : "");
			start = end;
		} while (start < line.size());
	}
}

/* Every bit the registers set lies in a field an AT_HWCAP or AT_HWCAP2 bit
 * is read from, so MRS cannot show the guest a feature its auxiliary vector
 * withholds. */
void require_every_field_read(const Advertised &advertised)
{
	for (std::size_t which = 0; which < feature_registers.size(); ++which) {
		std::uint64_t read = 0;
		for (const auto &bit : detail::hwcap_fields)
			if (bit.id_register == feature_registers[which])
				read |= ((std::uint64_t{1} << bit.width) - 1U)
					<< bit.shift;
		require((advertised.registers[which] & ~read) == 0,
			std::string{advertised.profile} +
				": no hwcap is read from a field a register sets");
	}
}

} // namespace

int main(int argc, char **argv)
{
	const auto all = profiles();
	if (argc == 2 && std::strcmp(argv[1], "--print") == 0) {
		print(all);
		return 0;
	}
	require(std::size(pinned) == all.size(),
		"the pinned table and the profiles differ in length");
	for (std::size_t index = 0; index < all.size(); ++index) {
		const auto observed = advertised(all[index]);
		const auto &expected = pinned[index];
		require(all[index].name == expected.profile,
			all[index].name + ": pinned as " + expected.profile);
		require(observed.hwcap == expected.hwcap &&
				observed.hwcap2 == expected.hwcap2,
			all[index].name + ": AT_HWCAP or AT_HWCAP2 changed");
		require(observed.registers == expected.registers,
			all[index].name +
				": an identification register changed");
		require_every_field_read(observed);
	}
	for (const auto &[profile_name, pinned_line] : pinned_features)
		require(features_line(advertised(find(all, profile_name))) ==
				pinned_line,
			std::string{profile_name} +
				": the Features line changed");

	/* The entry points the guest kernel calls apply this derivation to the
	 * probed host as the engines present it: outside a run, with the
	 * profile both engines implement (sme_host_for_support). */
	namespace arm64 = ::uml::cpu::arm64;
	const auto words = detail::guest_hwcaps(arm64::sme_host_for_support(
		::uml::platform::apple::probed_host_capabilities(),
		std::min(arm64::gadgets::sme_support,
			arm64::jit::sme_support)));
	require(darwin_uml_guest_hwcap() == words[0]);
	require(darwin_uml_guest_hwcap2() == words[1]);
	return 0;
}
