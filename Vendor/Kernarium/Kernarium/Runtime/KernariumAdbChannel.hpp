// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace kernarium::adb {

/* The kernel arguments through which the runtime tells an Android guest how
 * its adbd is reached. The launcher owns both: the release's command-line
 * file cannot set them (they are launcher prefixes). They become
 * ro.boot.kernarium.adb_key and ro.boot.kernarium.adb_tcp, which the guest's
 * adbd reads itself. android/lineage-24/README.md owns what the guest does
 * with them. */
inline constexpr std::string_view keyPrefix {"androidboot.kernarium.adb_key="};
inline constexpr std::string_view tcpPrefix {"androidboot.kernarium.adb_tcp="};

/* The longest adb public key the boot request may carry. adb's own 2048-bit
 * key is 700 characters. */
inline constexpr std::size_t maximumKeyLength = 1024;

/* Whether `key` is 1 through 1024 characters of the base64 alphabet. Anything
 * else could not be told apart from another kernel argument, or would not be a
 * key. */
[[nodiscard]] inline bool validPublicKey(std::string_view key) noexcept
{
    if (key.empty() || key.size() > maximumKeyLength)
        return false;
    for (const char character : key) {
        const bool allowed = (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '+' ||
            character == '/' || character == '=';
        if (!allowed)
            return false;
    }
    return true;
}

/* Whether a launch whose KERNARIUM_HOST_FORWARD is `forward` (null when unset)
 * asks the guest's adbd not to listen on TCP: it does so only when nothing on
 * the host can reach that port. A launch with a forward passes no
 * androidboot.kernarium.adb_tcp argument, and adbd keeps its TCP listener. */
[[nodiscard]] inline bool tcpDisabled(const char* forward) noexcept
{
    return forward == nullptr || forward[0] == '\0';
}

/* UML's COMMAND_LINE_SIZE (arch/um/include/asm/setup.h): the kernel joins its
 * arguments with spaces into a buffer this size, terminator included, and
 * exits the process when they do not fit. */
inline constexpr std::size_t kernelCommandLineSize = 4096;

/* The length of the kernel command line made of `arguments`: the arguments
 * after the program name, one space between each. */
[[nodiscard]] inline std::size_t kernelCommandLineLength(
    const std::vector<std::string>& arguments) noexcept
{
    std::size_t length = 0;
    for (std::size_t index = 1; index < arguments.size(); ++index)
        length += arguments[index].size() + (index > 1 ? 1U : 0U);
    return length;
}

/* Whether a command line of `length` bytes fits with its terminator. */
[[nodiscard]] inline bool fitsKernelCommandLine(std::size_t length) noexcept
{
    return length < kernelCommandLineSize;
}

/* The message for a command line that does not. */
[[nodiscard]] inline std::string commandLineTooLong(std::size_t length)
{
    return "the kernel command line is " + std::to_string(length) +
        " bytes and the kernel holds " +
        std::to_string(kernelCommandLineSize - 1U) +
        "; the adb key, the release's command line file and the launch's own "
        "arguments share it";
}

} // namespace kernarium::adb
