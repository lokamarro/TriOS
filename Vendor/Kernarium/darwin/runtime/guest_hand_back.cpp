// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_hand_back.hpp"

#include "guest_files.hpp"
#include "kernel_threads.hpp"
#include "memory/domain_heap.hpp"
#include "memory/shared_backing.hpp"
#include "native_kernel_layout.hpp"

namespace uml::runtime {

GuestHandBackBlock guest_hand_back_block() noexcept
{
	if (::uml::memory::guest_ram_borrowed())
		return GuestHandBackBlock::borrowed_ram;
	if (reclaim_ended_kernel_threads() != 0)
		return GuestHandBackBlock::kernel_threads;
	return GuestHandBackBlock::none;
}

bool hand_back_guest(GuestHandBack &hand_back) noexcept
{
	const auto window = process_native_window();
	hand_back.released = release_kernel_resources(window.begin, window.end);
	hand_back.guest_files = ::uml::platform::forget_guest_files();
	hand_back.heaps = ::uml::memory::DomainHeap::live_heaps();
	hand_back.heap_bytes = ::uml::memory::HeapRegion::instance().committed();
	hand_back.heap_leases = ::uml::memory::LeasedHeap::engine().leases();
	return ::uml::memory::hand_back_anonymous_guest_ram(
		window.end == 0 ? nullptr : reinterpret_cast<void *>(window.begin),
		window.end - window.begin) == 0;
}

} // namespace uml::runtime
