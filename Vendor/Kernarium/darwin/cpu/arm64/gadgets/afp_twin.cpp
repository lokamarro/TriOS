// SPDX-License-Identifier: GPL-2.0-or-later
#include "afp_twin.hpp"

#include <algorithm>
#include <vector>

extern "C" const uml::cpu::arm64::gadgets::AfpTwin uml_arm64_afp_twin_begin[];
extern "C" const uml::cpu::arm64::gadgets::AfpTwin uml_arm64_afp_twin_end[];

namespace uml::cpu::arm64::gadgets {
namespace {

/* The rows by base address, for a binary search per translated
 * instruction. Only a build for a host that implements FEAT_AFP consults it. */
class TwinIndex final {
public:
	TwinIndex()
		: rows_{afp_twins().begin(), afp_twins().end()}
	{
		std::ranges::sort(rows_, {}, &AfpTwin::base_begin);
	}

	[[nodiscard]] std::uintptr_t select(std::uintptr_t entry) const noexcept
	{
		const auto after = std::ranges::upper_bound(rows_, entry, {},
			&AfpTwin::base_begin);
		if (after == rows_.begin())
			return entry;
		const auto &row = *(after - 1);
		return entry < row.base_end ?
			row.twin_begin + (entry - row.base_begin) : entry;
	}

private:
	std::vector<AfpTwin> rows_;
};

} // namespace

std::span<const AfpTwin> afp_twins() noexcept
{
	return {uml_arm64_afp_twin_begin, uml_arm64_afp_twin_end};
}

/* Never destroyed, under the exit contract in docs/architecture.md, as the
 * forwarding contract indexes are. */
std::uintptr_t afp_twin(std::uintptr_t entry)
{
	[[clang::no_destroy]] static const TwinIndex index;
	return index.select(entry);
}

} // namespace uml::cpu::arm64::gadgets
