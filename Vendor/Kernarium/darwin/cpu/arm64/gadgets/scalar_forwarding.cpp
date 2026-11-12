// SPDX-License-Identifier: GPL-2.0-or-later
#include "scalar_forwarding.hpp"

#include "../layout/assembly_offsets.h"
#include <unordered_map>

namespace uml::cpu::arm64::gadgets {
namespace {
class ContractIndex final {
public:
	explicit ContractIndex(std::span<const ScalarContract> contracts)
	{
		entries_.reserve(contracts.size());
		for (const auto &entry : contracts)
			entries_.emplace(entry.entries[0], &entry);
	}
	ScalarAccess access(std::span<const std::uintptr_t> words) const
	{
		if (words.empty()) return {};
		const auto found = entries_.find(words[0]);
		if (found == entries_.end() || words.size() != 1U + found->second->operand_words)
			return {};
		const auto decode = [operand = words.size() > 1 ? words[1] : 0](std::uint16_t field) {
			if (field == scalar_no_register) return scalar_no_register;
			if ((field & 0x8000U) != 0) return static_cast<std::uint16_t>(field & 0x7fffU);
			if ((field & 0x4000U) != 0)
				return static_cast<std::uint16_t>(UML_GADGET_CPU_V0 +
					(((operand >> (field & 63U)) & 31U) << 4));
			const auto shift = field & 63U, width = (field >> 6) & 63U;
			return static_cast<std::uint16_t>(((operand >> shift) &
				((std::uint64_t{1} << width) - 1)) << ((field >> 12) & 3U));
		};
		const auto *contract = found->second;
		return {contract, decode(contract->destination),
			{decode(contract->sources[0]), decode(contract->sources[1]), decode(contract->sources[2])}};
	}
private:
	std::unordered_map<std::uintptr_t, const ScalarContract *> entries_;
};
} // namespace

extern "C" const ScalarContract uml_arm64_scalar_contract_begin[];
extern "C" const ScalarContract uml_arm64_scalar_contract_end[];

std::span<const ScalarContract> scalar_contracts() noexcept
{
	const auto bytes = reinterpret_cast<std::uintptr_t>(uml_arm64_scalar_contract_end) -
		reinterpret_cast<std::uintptr_t>(uml_arm64_scalar_contract_begin);
	return {uml_arm64_scalar_contract_begin, bytes / sizeof(ScalarContract)};
}

/* Never destroyed, under the exit contract in docs/architecture.md: carriers
 * translate with it until their process ends, and a host application can call
 * exit() while they still run. */
ScalarAccess scalar_access(std::span<const std::uintptr_t> words)
{
	[[clang::no_destroy]] static const ContractIndex index{scalar_contracts()};
	return index.access(words);
}

extern "C" const ScalarContract uml_arm64_vector_contract_begin[];
extern "C" const ScalarContract uml_arm64_vector_contract_end[];
std::span<const ScalarContract> vector_contracts() noexcept
{
	const auto bytes = reinterpret_cast<std::uintptr_t>(uml_arm64_vector_contract_end) -
		reinterpret_cast<std::uintptr_t>(uml_arm64_vector_contract_begin);
	return {uml_arm64_vector_contract_begin, bytes / sizeof(ScalarContract)};
}

/* Never destroyed, as the scalar index is. */
ScalarAccess vector_access(std::span<const std::uintptr_t> words)
{
	[[clang::no_destroy]] static const ContractIndex index{vector_contracts()};
	return index.access(words);
}
} // namespace uml::cpu::arm64::gadgets
