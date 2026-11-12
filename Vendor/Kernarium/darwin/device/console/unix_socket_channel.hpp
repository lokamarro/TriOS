// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "channel.hpp"

#include <filesystem>
#include <memory>

namespace uml::console {

class UnixSocketChannel final : public ConsoleChannel {
public:
    explicit UnixSocketChannel(std::filesystem::path socketPath);
    ~UnixSocketChannel() override;

    UnixSocketChannel(const UnixSocketChannel&) = delete;
    UnixSocketChannel& operator=(const UnixSocketChannel&) = delete;

    [[nodiscard]] std::vector<std::uint8_t> read(
        std::size_t maximumBytes) override;
    void write(std::span<const std::uint8_t> bytes) noexcept override;
    [[nodiscard]] int eventDescriptor() const noexcept override;
    void drainEventSource() override;
    void reset() noexcept override;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::console
