// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace kernarium::harness {

/* The kernel copies the whole initrd into boot memory before unpacking it,
 * and an archive larger than that memory panics the guest. */
inline constexpr std::uintmax_t maximumPayloadBytes = 64U << 20;

/* The file KERNARIUM_BOOT_PAYLOAD names in `documents`. The setting is a
 * plain file name; anything that could leave the directory is refused. */
[[nodiscard]] inline std::filesystem::path payloadPath(
    const std::filesystem::path& documents, std::string_view name)
{
    if (name.empty() || name == "." || name == ".." ||
        name.find('/') != std::string_view::npos) {
        throw std::invalid_argument(
            "KERNARIUM_BOOT_PAYLOAD must name a file in Documents");
    }
    return documents / std::string {name};
}

/* The payload's bytes: a newc segment the kernel can read after the
 * runtime's own, which uml-initramfs pads to four bytes. Throws
 * std::invalid_argument when the file is absent, too large or malformed. */
[[nodiscard]] inline std::vector<std::byte> readPayload(
    const std::filesystem::path& documents, std::string_view name)
{
    const auto path = payloadPath(documents, name);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        throw std::invalid_argument(
            "KERNARIUM_BOOT_PAYLOAD is not a regular file: " + path.string());
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximumPayloadBytes)
        throw std::invalid_argument(
            "KERNARIUM_BOOT_PAYLOAD exceeds 64 MiB: " + path.string());
    std::vector<std::byte> contents(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(reinterpret_cast<char*>(contents.data()),
            static_cast<std::streamsize>(contents.size())))
        throw std::invalid_argument(
            "KERNARIUM_BOOT_PAYLOAD cannot be read: " + path.string());
    constexpr std::string_view magic {"070701"};
    if (contents.size() < magic.size() || contents.size() % 4U != 0U ||
        std::memcmp(contents.data(), magic.data(), magic.size()) != 0)
        throw std::invalid_argument(
            "KERNARIUM_BOOT_PAYLOAD is not a padded newc segment: " +
            path.string());
    return contents;
}

/* KERNARIUM_PAYLOAD_REMOVE: "1" asks the runtime to delete the payload once it
 * holds the bytes, "0", empty or unset keeps the file. A launch that reuses
 * one copied payload must not ask. */
[[nodiscard]] inline bool removalRequested(const char* setting)
{
    const std::string_view value {setting != nullptr ? setting : ""};
    if (value.empty() || value == "0")
        return false;
    if (value == "1")
        return true;
    throw std::invalid_argument("KERNARIUM_PAYLOAD_REMOVE must be 0 or 1");
}

/* readPayload(), then, when `remove` is set, unlinks exactly that file. The
 * bytes are in memory by then, so a failed unlink does not fail the read: its
 * error is left in `removal`, and `removal` is clear otherwise. */
[[nodiscard]] inline std::vector<std::byte> takePayload(
    const std::filesystem::path& documents, std::string_view name, bool remove,
    std::error_code& removal)
{
    auto contents = readPayload(documents, name);
    removal.clear();
    if (remove) {
        const auto path = payloadPath(documents, name);
        if (::unlink(path.c_str()) != 0)
            removal = std::error_code {errno, std::generic_category()};
    }
    return contents;
}

} // namespace kernarium::harness
