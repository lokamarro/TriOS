// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "uml/emulator.h"
#include "require.hpp"
#include "testing.hpp"

#include <memory>
#include <source_location>

namespace uml::testing {

using Emulator = std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)>;

/* The handle `config` creates. The test fails at the caller when creation
 * does, or yields no handle. */
[[nodiscard]] inline Emulator create_emulator(
	const uml_emulator_config &config,
	const std::source_location location = std::source_location::current())
{
	uml_emulator *created = nullptr;
	require(uml_emulator_create(&config, &created) == UML_EMULATOR_OK &&
			created != nullptr,
		"could not create an emulator handle", location);
	return {created, uml_emulator_destroy};
}

/* A handle whose run enters the test kernel `entry`, through the emulator test
 * variant, with the injected `network` if there is one. No test kernel opens
 * the rootfs, so its path only names the handle's disk. The handle owns a copy
 * of the kernel image. */
[[nodiscard]] inline Emulator make_emulator(
	::uml::emulator::testing::KernelEntry entry,
	const uml_emulator_network_callbacks *network = nullptr,
	const std::source_location location = std::source_location::current())
{
	const auto image = ::uml::emulator::testing::kernel_image(entry);
	return create_emulator(
		{
			.abi_version = UML_EMULATOR_ABI_VERSION,
			.struct_size = sizeof(uml_emulator_config),
			.rootfs_path = "/tmp/uml-test.img",
			.kernel_image = image.data(),
			.kernel_image_size = image.size(),
			.network = network,
		},
		location);
}

} // namespace uml::testing
