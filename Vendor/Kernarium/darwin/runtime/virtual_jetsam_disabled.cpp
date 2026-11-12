// SPDX-License-Identifier: GPL-2.0-or-later
#include "virtual_jetsam.hpp"

/*
 * The virtual Jetsam of an App Store runtime: there is none. The watch
 * (virtual_jetsam.cpp) writes a line when a Mac run's footprint crosses a
 * limit its launch stated; iOS sets its own limit, so the watch never starts
 * there. A runtime built with UML_APP_STORE_RUNTIME links this file in its
 * place, so the sampler thread and its lines are not in it.
 */
namespace uml::runtime {

struct VirtualJetsamWatch::State final {};

VirtualJetsamWatch::~VirtualJetsamWatch() = default;

std::unique_ptr<VirtualJetsamWatch> start_virtual_jetsam_watch(std::size_t)
{
	return nullptr;
}

} // namespace uml::runtime
