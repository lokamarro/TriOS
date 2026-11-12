// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "uml_darwin_abi.h"

namespace uml::testing {

/* An arm64 task's register tail as the kernel hands it to
 * darwin_uml_enter_guest(): the FP registers, then the SME registers, so
 * sizeof() is the fp_size the arm64 path requires. The FP members keep their
 * names, as in the kernel's own view of the tail. */
struct GuestFpTail final : darwin_uml_guest_fp_registers {
	darwin_uml_guest_sme_registers sme;
};

static_assert(sizeof(GuestFpTail) == DARWIN_UML_ABI_GUEST_ARM64_TAIL_SIZE);
static_assert(sizeof(darwin_uml_guest_fp_registers) ==
	      DARWIN_UML_ABI_GUEST_SME_REGISTERS_OFFSET);
static_assert(sizeof(darwin_uml_guest_fp_registers) +
		      sizeof(darwin_uml_guest_sme_registers) ==
	      DARWIN_UML_ABI_GUEST_ARM64_TAIL_SIZE);

} // namespace uml::testing
