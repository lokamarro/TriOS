// SPDX-License-Identifier: GPL-2.0-or-later
/* Group "absent": an instruction of an extension the process does not see in
 * its HWCAP words is UNDEFINED: SIGILL with the instruction's address, from
 * A32 and from T32: VFP and Advanced SIMD where the HWCAP words leave them
 * out, and the crypto, CRC32, dot-product and half-precision groups the
 * HWCAP bits leave out. Each SIGILL is counted as expected. The VFP and NEON
 * state registers that user mode cannot read are covered by the simd group. */
#include "engine_probe.hpp"

extern "C" {
#define AB(name) void name##_arm(probe::word, probe::word); void name##_thumb(probe::word, probe::word);
AB(ab_vadd) AB(ab_vmrs) AB(ab_vmsr) AB(ab_vldr) AB(ab_vmov_core) AB(ab_vcvt)
AB(ab_vadd_i32) AB(ab_vld1) AB(ab_vmov_imm) AB(ab_vdup) AB(ab_vrintn) AB(ab_vmaxnm)
AB(ab_aese) AB(ab_aesd) AB(ab_aesmc) AB(ab_aesimc) AB(ab_pmull) AB(ab_sha1c) AB(ab_sha1h)
AB(ab_sha256h) AB(ab_crc32b) AB(ab_crc32cw) AB(ab_vsdot) AB(ab_fp16_add) AB(ab_fp16_vec)
AB(ab_vfmal) AB(ab_bf16_dot) AB(ab_i8mm)
#undef AB
}

namespace probe {
namespace {

enum Extension { vfp, neon, aes, pmull, sha1, sha2, crc32, dotprod, fphp, asimdhp, fhm, bf16, i8mm };

struct Case {
	void (*routine)(word, word);
	Extension extension;
	const char *name;
};

#define BOTH(routine, extension, text) \
	{routine##_arm, extension, "A32 " text}, {routine##_thumb, extension, "T32 " text}

const Case cases[] = {
	BOTH(ab_vadd, vfp, "VADD.F32"),
	BOTH(ab_vmrs, vfp, "VMRS FPSCR"),
	BOTH(ab_vmsr, vfp, "VMSR FPSCR"),
	BOTH(ab_vldr, vfp, "VLDR"),
	BOTH(ab_vmov_core, vfp, "VMOV Sn, Rt"),
	BOTH(ab_vcvt, vfp, "VCVT.F64.F32"),
	BOTH(ab_vrintn, vfp, "VRINTN"),
	BOTH(ab_vmaxnm, vfp, "VMAXNM"),
	BOTH(ab_vadd_i32, neon, "VADD.I32"),
	BOTH(ab_vld1, neon, "VLD1"),
	BOTH(ab_vmov_imm, neon, "VMOV.I32 immediate"),
	BOTH(ab_vdup, neon, "VDUP"),
	BOTH(ab_aese, aes, "AESE"),
	BOTH(ab_aesd, aes, "AESD"),
	BOTH(ab_aesmc, aes, "AESMC"),
	BOTH(ab_aesimc, aes, "AESIMC"),
	BOTH(ab_pmull, pmull, "VMULL.P64"),
	BOTH(ab_sha1c, sha1, "SHA1C"),
	BOTH(ab_sha1h, sha1, "SHA1H"),
	BOTH(ab_sha256h, sha2, "SHA256H"),
	BOTH(ab_crc32b, crc32, "CRC32B"),
	BOTH(ab_crc32cw, crc32, "CRC32CW"),
	BOTH(ab_vsdot, dotprod, "VSDOT"),
	BOTH(ab_fp16_add, fphp, "VADD.F16 (scalar)"),
	BOTH(ab_fp16_vec, asimdhp, "VADD.F16 (vector)"),
	BOTH(ab_vfmal, fhm, "VFMAL.F16"),
	BOTH(ab_bf16_dot, bf16, "VDOT.BF16"),
	BOTH(ab_i8mm, i8mm, "VUSDOT"),
};

constexpr word hwcap_vfp_bit = 1U << 6;
bool advertised(Extension extension)
{
	switch (extension) {
	case vfp: return (hwcap & hwcap_vfp_bit) != 0;
	case neon: return (hwcap & hwcap_neon) != 0;
	case aes: return (hwcap2 & hwcap2_aes) != 0;
	case pmull: return (hwcap2 & hwcap2_pmull) != 0;
	case sha1: return (hwcap2 & hwcap2_sha1) != 0;
	case sha2: return (hwcap2 & hwcap2_sha2) != 0;
	case crc32: return (hwcap2 & hwcap2_crc32) != 0;
	case dotprod: return (hwcap & hwcap_asimddp) != 0;
	case fphp: return (hwcap & hwcap_fphp) != 0;
	case asimdhp: return (hwcap & hwcap_asimdhp) != 0;
	case fhm: return (hwcap & hwcap_asimdfhm) != 0;
	case bf16: return (hwcap & hwcap_asimdbf16) != 0;
	case i8mm: return (hwcap & hwcap_i8mm) != 0;
	}
	return true;
}

const char *labelled(char *buffer, const char *prefix, const char *text)
{
	word length = 0;
	for (const char *p = prefix; *p; ++p)
		buffer[length++] = *p;
	for (const char *p = text; *p; ++p)
		buffer[length++] = *p;
	buffer[length] = 0;
	return buffer;
}

} // namespace

void group_absent()
{
	char name[160];
	const word area = mmap_pages(page_size, prot_read | prot_write);
	for (const Case &item : cases) {
		if (advertised(item.extension))
			continue;
		catch_signal(sigill, return_to_lr);
		item.routine(area, 0x12345678);
		release_signal(sigill);
		check_eq(caught.count, 1, labelled(name, item.name, " is UNDEFINED: SIGILL"));
		if (caught.count == 1 && !under_qemu()) {
			check_eq(caught.pc, addr(item.routine) & ~1UL, labelled(name, item.name, ": the signal pc is the instruction"));
			check_eq(caught.address, addr(item.routine) & ~1UL, labelled(name, item.name, ": si_addr is the instruction"));
			check_eq(static_cast<u32>(caught.code), 1, labelled(name, item.name, ": si_code is ILL_ILLOPC"));
		}
	}
}

} // namespace probe
