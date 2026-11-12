// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "uml_darwin_static_mmu.h"

#include <cstdint>

/* Test conveniences for reading a generation's change sequences without
 * holding an execution context. Engines keep a context per vCPU and read
 * darwin_uml_static_vm_execution_context_sequences() from it; these helpers
 * compose the same read from a short-lived context, so they report what an
 * engine would see and add nothing to the production bridge. The
 * execution_interrupt field is always 0: it compares against the interrupt
 * count a context captured when it entered, and these contexts never enter.
 */
extern "C" {

struct darwin_uml_static_sequences darwin_uml_static_vm_sequences(
	unsigned long generation);
std::uint64_t darwin_uml_static_vm_code_changes(unsigned long generation);

}
