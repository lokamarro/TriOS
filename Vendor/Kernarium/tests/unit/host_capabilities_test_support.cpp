// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/host_capabilities.hpp"
#include "platform/apple/host_features.hpp"

#include <cstdlib>

namespace {

class TestHostCapabilities final {
public:
	TestHostCapabilities()
		: capabilities_{::uml::platform::apple::probed_host_capabilities()}
	{
		if (!uml::cpu::install_process_host_capabilities(capabilities_))
			std::abort();
	}

private:
	const uml::cpu::HostCapabilities capabilities_;
};

const TestHostCapabilities test_host_capabilities;

} // namespace
