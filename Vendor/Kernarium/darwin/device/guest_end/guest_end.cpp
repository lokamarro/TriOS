// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_end.hpp"

#include <array>
#include <cstddef>
#include <mutex>
#include <stdexcept>

namespace uml::device {
namespace {

/* One process serves one guest at a time, with a handful of devices that hold
 * guest requests; the unit tests build a few services side by side. */
constexpr std::size_t maximum_callbacks = 8;

struct Slot final {
	GuestEndRegistration::Callback callback{};
	void *context{};
};

/* Constant-initialized, so no guard variable stands on the path of a report
 * from the guest. The callbacks run under the lock, which makes unregistering
 * wait for a callback that is running. */
std::mutex table_mutex;
std::array<Slot, maximum_callbacks> table{};

} // namespace

GuestEndRegistration::GuestEndRegistration(Callback callback, void *context)
{
	if (callback == nullptr)
		throw std::invalid_argument("guest end callback is null");
	std::lock_guard lock{table_mutex};
	for (std::size_t index = 0; index < table.size(); ++index) {
		if (table[index].callback != nullptr)
			continue;
		table[index] = {callback, context};
		slot_ = static_cast<int>(index);
		return;
	}
	throw std::length_error("no free guest end callback slot");
}

GuestEndRegistration::~GuestEndRegistration()
{
	std::lock_guard lock{table_mutex};
	table[static_cast<std::size_t>(slot_)] = {};
}

void notify_guest_end() noexcept
{
	std::lock_guard lock{table_mutex};
	for (const auto &slot : table) {
		if (slot.callback != nullptr)
			slot.callback(slot.context);
	}
}

} // namespace uml::device
