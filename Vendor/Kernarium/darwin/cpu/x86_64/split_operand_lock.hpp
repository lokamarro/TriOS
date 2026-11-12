// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <mutex>

namespace uml::cpu::x86_64 {

/* Serializes every locked operand that no exclusive access can cover: one
 * that crosses a 16-byte granule. Such operands are rare enough that one lock
 * for the process does not contend. Every x86 engine takes this one lock, so
 * such an operand is atomic against the same operand on any vCPU. */
extern std::mutex split_operand_lock;

} // namespace uml::cpu::x86_64
