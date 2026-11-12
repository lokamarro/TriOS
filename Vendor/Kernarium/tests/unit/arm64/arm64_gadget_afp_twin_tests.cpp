// SPDX-License-Identifier: GPL-2.0-or-later
/* The FEAT_AFP twin table and its selection. The twins' results under every
 * other FPCR control are the corpus's business (arm64-instruction-corpus
 * with FEAT_AFP assumed); this checks the table's shape, which entry the
 * builder emits on each kind of host, the FPCR accesses the twins keep
 * FIZ, AH and NEP through, and the identification fields that follow the
 * same predicates. */
#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/arm64/floating_controls.hpp"
#include "cpu/arm64/gadgets/afp_twin.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/id_registers.hpp"
#include "platform/apple/host_features.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;

extern "C" {
extern const char uml_arm64_gadget_bank_begin[];
extern const char uml_arm64_gadget_bank_end[];
extern const char uml_arm64_gadget_afp_section_begin[];
extern const char uml_arm64_gadget_afp_section_end[];
void uml_arm64_gadget_floating_add_single();
void uml_arm64_gadget_floating_add_single_afp();
void uml_arm64_gadget_floating_sqrt_double();
void uml_arm64_gadget_floating_sqrt_double_afp();
void uml_arm64_gadget_sysreg_fpcr_write_r();
void uml_arm64_gadget_sysreg_fpcr_write_r_afp();
void uml_arm64_gadget_sysreg_fpcr_write_z();
void uml_arm64_gadget_sysreg_fpsr_write_r();
void uml_arm64_gadget_gpr_to_floating_fixed_scvtf_w_s0();
void uml_arm64_gadget_gpr_to_floating_fixed_scvtf_w_s0_end();
void uml_arm64_gadget_gpr_to_floating_fixed_scvtf_w_s0_afp();
void uml_arm64_gadget_illegal();
}

namespace {

using uml::testing::require;

template<typename Entry>
[[nodiscard]] std::uintptr_t address(Entry entry) noexcept
{
	return reinterpret_cast<std::uintptr_t>(entry);
}

constexpr std::uint32_t fadd_s0_s1_s2 = 0x1e22'2820U;
constexpr std::uint32_t msr_fpcr_x1 = 0xd51b'4401U;
constexpr std::uint32_t mrs_x2_fpcr = 0xd53b'4402U;
constexpr std::uint32_t svc_0 = 0xd400'0001U;

/* The probed host with FEAT_AFP and every host feature implements_afp()
 * needs. Translation alone never runs the twins on hardware without AFP;
 * the FPCR accesses below run on any host. */
[[nodiscard]] uml::cpu::HostCapabilities afp_host()
{
	auto host = ::uml::platform::apple::probed_host_capabilities();
	for (const auto feature : {uml::cpu::ArmFeature::afp,
		     uml::cpu::ArmFeature::fp16, uml::cpu::ArmFeature::fhm,
		     uml::cpu::ArmFeature::bf16, uml::cpu::ArmFeature::frintts,
		     uml::cpu::ArmFeature::fcma, uml::cpu::ArmFeature::jscvt})
		host.supported.set(static_cast<std::size_t>(feature));
	host.fp16 = true;
	return host;
}

[[nodiscard]] uml::cpu::HostCapabilities without_afp(
	uml::cpu::HostCapabilities host)
{
	host.supported.reset(static_cast<std::size_t>(uml::cpu::ArmFeature::afp));
	return host;
}

void table_has_one_equal_stride_twin_per_base()
{
	const auto rows = gadgets::afp_twins();
	require(!rows.empty());
	std::vector<gadgets::AfpTwin> sorted{rows.begin(), rows.end()};
	std::ranges::sort(sorted, {}, &gadgets::AfpTwin::base_begin);
	const auto bank_begin = address(uml_arm64_gadget_bank_begin);
	const auto bank_end = address(uml_arm64_gadget_bank_end);
	const auto twins_begin = address(uml_arm64_gadget_afp_section_begin);
	const auto twins_end = address(uml_arm64_gadget_afp_section_end);
	for (std::size_t index = 0; index < sorted.size(); ++index) {
		const auto &row = sorted[index];
		require(row.base_begin < row.base_end);
		require(row.base_end - row.base_begin ==
			row.twin_end - row.twin_begin);
		require(row.base_begin >= bank_begin && row.base_end <= bank_end);
		require(row.twin_begin >= twins_begin && row.twin_end <= twins_end);
		if (index != 0)
			require(sorted[index - 1].base_end <= row.base_begin);
	}
	std::ranges::sort(sorted, {}, &gadgets::AfpTwin::twin_begin);
	for (std::size_t index = 1; index < sorted.size(); ++index)
		require(sorted[index - 1].twin_end <= sorted[index].twin_begin);
}

void lookup_maps_entries_and_table_offsets()
{
	require(gadgets::afp_twin(address(uml_arm64_gadget_floating_add_single)) ==
		address(uml_arm64_gadget_floating_add_single_afp));
	require(gadgets::afp_twin(address(uml_arm64_gadget_floating_sqrt_double)) ==
		address(uml_arm64_gadget_floating_sqrt_double_afp));
	require(gadgets::afp_twin(address(uml_arm64_gadget_sysreg_fpcr_write_r)) ==
		address(uml_arm64_gadget_sysreg_fpcr_write_r_afp));
	/* XZR writes clear FPCR on every host, and FPSR has no AFP field. */
	require(gadgets::afp_twin(address(uml_arm64_gadget_sysreg_fpcr_write_z)) ==
		address(uml_arm64_gadget_sysreg_fpcr_write_z));
	require(gadgets::afp_twin(address(uml_arm64_gadget_sysreg_fpsr_write_r)) ==
		address(uml_arm64_gadget_sysreg_fpsr_write_r));
	require(gadgets::afp_twin(address(uml_arm64_gadget_illegal)) ==
		address(uml_arm64_gadget_illegal));

	/* SCVTF Sd, Wn, #fbits for fbits 1..32: the third entry is #3. */
	const auto table = address(uml_arm64_gadget_gpr_to_floating_fixed_scvtf_w_s0);
	const auto stride =
		(address(uml_arm64_gadget_gpr_to_floating_fixed_scvtf_w_s0_end) -
		 table) / 32U;
	require(stride != 0U);
	require(gadgets::afp_twin(table + 2U * stride) ==
		address(uml_arm64_gadget_gpr_to_floating_fixed_scvtf_w_s0_afp) +
			2U * stride);
}

[[nodiscard]] gadgets::BuildResult build(const uml::cpu::HostCapabilities &host,
	std::span<const std::uint32_t> instructions)
{
	return gadgets::Builder{}.build(0x8000, instructions, {},
		std::pmr::get_default_resource(), {},
		gadgets::BuildOptions{.host_capabilities = host});
}

void builder_selects_twins_only_where_afp_is_implemented()
{
	const std::array program{fadd_s0_s1_s2, svc_0};
	const auto afp = afp_host();
	require(arm64::implements_afp(afp));
	require(!arm64::implements_afp(without_afp(afp)));

	const auto twin = build(afp, program);
	require(twin.complete);
	require(twin.program.words().front() ==
		address(uml_arm64_gadget_floating_add_single_afp));
	const auto base = build(without_afp(afp), program);
	require(base.complete);
	require(base.program.words().front() ==
		address(uml_arm64_gadget_floating_add_single));
}

[[nodiscard]] arm64::CpuState run(const uml::cpu::HostCapabilities &host,
	std::uint64_t written)
{
	const std::array program{msr_fpcr_x1, mrs_x2_fpcr, svc_0};
	const auto built = build(host, program);
	require(built.complete);
	arm64::CpuState cpu{};
	cpu.pc = 0x8000;
	cpu.registers[1] = written;
	::uml::memory::Tlb tlb;
	require(gadgets::execute(built.program, cpu, tlb) ==
		::uml::cpu::EngineEvent::system_call);
	return cpu;
}

void fpcr_keeps_the_alternate_controls_where_implemented()
{
	constexpr std::uint64_t written = 0xffff'ffff'ffff'ffffULL &
		~std::uint64_t{arm64::detail::fpcr_exception_enable_mask};
	const auto afp = afp_host();
	const auto kept = run(afp, written);
	require(kept.fpcr == (written & arm64::fpcr_writable_mask_for(afp)));
	require((kept.fpcr & arm64::fpcr_afp_controls) ==
		arm64::fpcr_afp_controls);
	require(kept.registers[2] == kept.fpcr);

	const auto masked = run(without_afp(afp), written);
	require(masked.fpcr == (written & arm64::fpcr_writable_mask));
	require(masked.registers[2] == masked.fpcr);
}

void identification_follows_the_predicate()
{
	constexpr std::uint32_t mrs_isar2 = 0xd538'0640U;
	constexpr std::uint32_t mrs_mmfr1 = 0xd538'0720U;
	auto afp = afp_host();
	afp.supported.set(static_cast<std::size_t>(uml::cpu::ArmFeature::rpres));
	{
		const ::uml::cpu::HostCapabilitiesScope scope{afp};
		require(((arm64::detail::emulated_id_register_value(mrs_mmfr1) >>
			44U) & 15U) == 1U);
		require(((arm64::detail::emulated_id_register_value(mrs_isar2) >>
			4U) & 15U) == 1U);
	}
	const auto none = without_afp(afp);
	{
		const ::uml::cpu::HostCapabilitiesScope scope{none};
		require(arm64::detail::emulated_id_register_value(mrs_mmfr1) == 0U);
		require(((arm64::detail::emulated_id_register_value(mrs_isar2) >>
			4U) & 15U) == 0U);
		require(!arm64::implements_rpres(none));
	}
}

} // namespace

int main()
{
	table_has_one_equal_stride_twin_per_base();
	lookup_maps_entries_and_table_offsets();
	builder_selects_twins_only_where_afp_is_implemented();
	fpcr_keeps_the_alternate_controls_where_implemented();
	identification_follows_the_predicate();
	std::printf("arm64 gadget FEAT_AFP twin tests passed (%zu rows)\n",
		gadgets::afp_twins().size());
	return 0;
}
