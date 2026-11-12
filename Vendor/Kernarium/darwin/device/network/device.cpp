// SPDX-License-Identifier: GPL-2.0-or-later
#include "device.hpp"

#include "run_routing.hpp"
#include "uml_darwin_network.h"

#include <cerrno>
namespace uml::network {

Provider resolve_active_provider() noexcept
{
	const auto route = ::uml::routing::current_route();
	const auto *context = static_cast<const ProviderContext *>(route.network);
	return context == nullptr ? Provider{} : context->provider();
}

} // namespace uml::network

extern "C" int darwin_uml_net_open(unsigned char mac[6], unsigned int *mtu)
{
	if (mac == nullptr || mtu == nullptr)
		return -EINVAL;
	try {
		const auto provider = uml::network::resolve_active_provider();
		return provider.open == nullptr ? -ENETDOWN :
			provider.open(provider.context, mac, mtu);
	} catch (...) {
		return -EIO;
	}
}

extern "C" int darwin_uml_net_irq_fd(void)
{
	try {
		const auto provider = uml::network::resolve_active_provider();
		return provider.irq_fd == nullptr ? -1 :
			provider.irq_fd(provider.context);
	} catch (...) {
		return -1;
	}
}

extern "C" int darwin_uml_net_read(void *buffer, unsigned long capacity)
{
	if (buffer == nullptr && capacity != 0)
		return -EINVAL;
	try {
		const auto provider = uml::network::resolve_active_provider();
		return provider.read == nullptr ? -ENETDOWN :
			provider.read(provider.context, buffer, capacity);
	} catch (...) {
		return -EIO;
	}
}

extern "C" int darwin_uml_net_write(const void *buffer, unsigned long length)
{
	if (buffer == nullptr && length != 0)
		return -EINVAL;
	try {
		const auto provider = uml::network::resolve_active_provider();
		return provider.write == nullptr ? -ENETDOWN :
			provider.write(provider.context, buffer, length);
	} catch (...) {
		return -EIO;
	}
}
