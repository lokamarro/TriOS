// SPDX-License-Identifier: GPL-2.0-or-later
#include "static_mmu_test_support.hpp"

extern "C" struct darwin_uml_static_sequences darwin_uml_static_vm_sequences(
	unsigned long generation)
{
	void *context = darwin_uml_static_vm_execution_context_create(generation);
	if (context == nullptr)
		return {};
	auto sequences = darwin_uml_static_vm_execution_context_sequences(context);
	darwin_uml_static_vm_execution_context_destroy(context);
	sequences.execution_interrupt = 0;
	return sequences;
}

extern "C" std::uint64_t darwin_uml_static_vm_code_changes(
	unsigned long generation)
{
	return darwin_uml_static_vm_sequences(generation).code_changes;
}
