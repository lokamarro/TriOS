// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace kernarium::adb {

/* What an automation launch may say about the guest's ADB authorization. Only
 * the automation Android runtime (KERNARIUM_AUTOMATION_ADB_AUTH) reads it; the
 * release runtime carries neither the name nor this decision. */
inline constexpr std::string_view requestVariable {
    "KERNARIUM_ANDROID_ADB_AUTH"};

/* Whether every address a KERNARIUM_HOST_FORWARD specification would listen
 * on is loopback. The grammar is the SLiRP provider's
 * ("[PROTO:][HOSTADDR:]HOSTPORT-[GUESTADDR:]GUESTPORT", the host address
 * defaulting to loopback); anything this does not recognise is not loopback. */
[[nodiscard]] inline bool forwardListensOnLoopbackOnly(
    std::string_view specification)
{
    if (specification.starts_with("tcp:") || specification.starts_with("udp:"))
        specification.remove_prefix(4);
    const auto split = specification.find('-');
    if (split == std::string_view::npos)
        return false;
    const auto host = specification.substr(0, split);
    const auto separator = host.rfind(':');
    const auto port = separator == std::string_view::npos
        ? host : host.substr(separator + 1);
    if (port.empty() || port.find_first_not_of("0123456789") !=
            std::string_view::npos)
        return false;
    if (separator == std::string_view::npos)
        return true;
    const std::string address {host.substr(0, separator)};
    in_addr parsed {};
    if (inet_pton(AF_INET, address.c_str(), &parsed) != 1)
        return false;
    return (ntohl(parsed.s_addr) >> 24) == 127U;
}

/* The androidboot.kernarium.adb_auth value for one launch. `request` is the
 * KERNARIUM_ANDROID_ADB_AUTH setting and `forward` the launch's
 * KERNARIUM_HOST_FORWARD (null when unset). "1", empty or unset keeps the
 * stock authorization prompt, "0" turns it off. Off gives any process that
 * reaches the forward an unauthenticated shell (and root on a userdebug
 * diagnostics image, where it also starts the guest's adb_root service): it is
 * refused unless the launch publishes no host port or only a loopback one,
 * which a host reaches over USB and no network can. */
[[nodiscard]] inline std::string_view authorizationValue(
    const char* request, const char* forward)
{
    const std::string_view value {request != nullptr ? request : ""};
    if (value.empty() || value == "1")
        return "1";
    if (value != "0")
        throw std::invalid_argument(
            "KERNARIUM_ANDROID_ADB_AUTH must be 0 or 1");
    if (forward != nullptr && forward[0] != '\0' &&
        !forwardListensOnLoopbackOnly(forward))
        throw std::invalid_argument(
            "KERNARIUM_ANDROID_ADB_AUTH=0 needs KERNARIUM_HOST_FORWARD to "
            "listen on loopback only");
    return "0";
}

} // namespace kernarium::adb
