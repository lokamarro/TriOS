// SPDX-License-Identifier: GPL-2.0-or-later
#include "address_layout.hpp"

#include <atomic>
#include <limits>

namespace uml::memory {
namespace {

struct AddressWindowState final {
	std::atomic_uint64_t revision{};
	std::atomic_uint64_t start{DARWIN_UML_NATIVE_ADDRESS_START};
	std::atomic_uint64_t end{DARWIN_UML_NATIVE_ADDRESS_END};
	std::atomic_flag writer = ATOMIC_FLAG_INIT;
};

AddressWindowState configured_window;

struct AddressWindow final {
	std::uint64_t start;
	std::uint64_t end;
};

[[nodiscard]] AddressWindow snapshot_window() noexcept
{
	for (;;) {
		const auto before = configured_window.revision.load(
			std::memory_order_acquire);
		if ((before & 1U) != 0U)
			continue;
		const AddressWindow window{
			configured_window.start.load(std::memory_order_relaxed),
			configured_window.end.load(std::memory_order_relaxed),
		};
		/* Keeps both loads ahead of the closing check; it pairs with
		 * the writer's release fence. */
		std::atomic_thread_fence(std::memory_order_acquire);
		if (configured_window.revision.load(std::memory_order_relaxed) ==
		    before)
			return window;
	}
}

void publish_window(std::uint64_t start, std::uint64_t end) noexcept
{
	while (configured_window.writer.test_and_set(std::memory_order_acquire))
		;
	configured_window.revision.fetch_add(1, std::memory_order_acq_rel);
	/* A reader that loads either store below then sees the odd revision. */
	std::atomic_thread_fence(std::memory_order_release);
	configured_window.start.store(start, std::memory_order_relaxed);
	configured_window.end.store(end, std::memory_order_relaxed);
	configured_window.revision.fetch_add(1, std::memory_order_release);
	configured_window.writer.clear(std::memory_order_release);
}

[[nodiscard]] AddressDomain classify_with_window(std::uint64_t address,
	std::uint64_t length, const AddressWindow &window) noexcept
{
	enum class Segment : std::uint8_t {
		guest_low,
		native_window,
		guest_high,
		native_high,
	};
	if (length > std::numeric_limits<std::uint64_t>::max() - address)
		return AddressDomain::invalid;
	const auto last = length == 0 ? address : address + length - 1U;
	const auto segment = [&](std::uint64_t value) {
		if (value < window.start)
			return Segment::guest_low;
		if (value < window.end)
			return Segment::native_window;
		if (value < DARWIN_UML_GUEST_ADDRESS_LIMIT)
			return Segment::guest_high;
		return Segment::native_high;
	};
	const auto first_segment = segment(address);
	if (first_segment != segment(last))
		return AddressDomain::invalid;
	return first_segment == Segment::guest_low ||
		first_segment == Segment::guest_high ? AddressDomain::guest :
		AddressDomain::native;
}

} // namespace

AddressDomain ConfiguredAddressLayout::classify(
	std::uint64_t address, std::uint64_t length) noexcept
{
	return classify_with_window(address, length, snapshot_window());
}

bool ConfiguredAddressLayout::is_guest(std::uint64_t address) noexcept
{
	return classify(address, 1) == AddressDomain::guest;
}

bool ConfiguredAddressLayout::is_native_window(
	std::uint64_t address, std::uint64_t length) noexcept
{
	if (length > std::numeric_limits<std::uint64_t>::max() - address)
		return false;
	const auto window = snapshot_window();
	const auto last = length == 0 ? address : address + length - 1U;
	return address >= window.start && address < window.end &&
		last >= window.start && last < window.end;
}

bool configure_native_address_window(
	std::uint64_t start, std::uint64_t end) noexcept
{
	if (start > end || start < DARWIN_UML_NATIVE_ADDRESS_START ||
	    end > DARWIN_UML_GUEST_ADDRESS_LIMIT)
		return false;
	publish_window(start, end);
	return true;
}

void reset_native_address_window() noexcept
{
	publish_window(DARWIN_UML_NATIVE_ADDRESS_START,
		DARWIN_UML_NATIVE_ADDRESS_END);
}

} // namespace uml::memory
