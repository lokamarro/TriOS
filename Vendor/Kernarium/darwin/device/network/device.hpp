// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

namespace uml::network {

struct Provider final {
	void *context{};
	int (*open)(void *, unsigned char[6], unsigned int *){};
	int (*irq_fd)(void *){};
	int (*read)(void *, void *, std::size_t){};
	int (*write)(void *, const void *, std::size_t){};
};

[[nodiscard]] Provider resolve_active_provider() noexcept;

class ProviderContext final {
public:
	explicit ProviderContext(Provider provider) noexcept : provider_{provider} {}

	[[nodiscard]] Provider provider() const noexcept { return provider_; }

private:
	Provider provider_;
};

} // namespace uml::network
