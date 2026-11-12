// SPDX-License-Identifier: GPL-2.0-or-later
/* Prints how much of the SME profile each execution engine implements, as
 * "gadgets=<level> jit=<level>", for tests/integration/sme/run_guest_sme.py
 * to decide whether a guest SME test can run on an engine. */

#include "cpu/arm64/gadgets/sme_support.hpp"
#include "cpu/arm64/jit/jit_sme_support.hpp"

#include <cstdio>

namespace {

const char *level(uml::cpu::arm64::SmeSupport support)
{
	switch (support) {
	case uml::cpu::arm64::SmeSupport::none:
		return "none";
	case uml::cpu::arm64::SmeSupport::control:
		return "control";
	case uml::cpu::arm64::SmeSupport::complete:
		return "complete";
	case uml::cpu::arm64::SmeSupport::sme2:
		return "sme2";
	}
	return "unknown";
}

} // namespace

int main()
{
	std::printf("gadgets=%s jit=%s\n",
		level(uml::cpu::arm64::gadgets::sme_support),
		level(uml::cpu::arm64::jit::sme_support));
	return 0;
}
