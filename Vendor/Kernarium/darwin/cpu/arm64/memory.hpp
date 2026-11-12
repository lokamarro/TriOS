// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* Compatibility names for the ISA implementation. The canonical guest
 * address-space contract lives in the neutral memory layer. */
#include "memory/guest_address_space.hpp"

namespace uml::cpu::arm64 {

using MemoryAccess = ::uml::memory::MemoryAccess;
using MemoryFault = ::uml::memory::MemoryFault;
using GuestMemory = ::uml::memory::GuestAddressSpace;

} // namespace uml::cpu::arm64
