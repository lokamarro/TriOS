/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <string_view>

namespace uml::cpu {

/* The name this virtual CPU answers to. An x86-64 guest has two ways to ask:
 * it can read the model name in /proc/cpuinfo, which the kernel fills from
 * `darwin_uml_guest_cpu_model_name()`, or it can execute CPUID and read the
 * brand string out of leaves 0x80000002 through 0x80000004. Both answers
 * originate here. An arm64 guest's /proc/cpuinfo names no model, as on arm64
 * Linux, which prints one only to a 32-bit task.
 *
 * The emulator is the authority because it is the component that decides what
 * the virtual CPU is; a kernel slice reports that decision rather than making
 * one of its own. The kernels also print it as the SMBIOS processor version.
 * The x86-64 gadget engine's CPUID gadget (gadgets/engine/system.inc) spells
 * the bytes out in assembly, and `x86-64-gadget-bootstrap` requires them to
 * match. The Android image names the same CPU as `ro.soc.manufacturer` and
 * `ro.soc.model`, which a space joins, and a tooling test requires that to
 * match too.
 *
 * The array is the definition so the bridge can hand the kernel a plain
 * NUL-terminated pointer into it; the view is what host code reads.
 */
inline constexpr char guest_cpu_model_name_storage[] = "Kernarium Virtual CPU";

inline constexpr std::string_view guest_cpu_model_name{
	guest_cpu_model_name_storage};

/* CPUID has room for 48 brand bytes and requires a NUL terminator inside
 * them. */
static_assert(sizeof guest_cpu_model_name_storage <= 48U,
	"the CPUID brand string holds 48 bytes including its terminator");

} // namespace uml::cpu
