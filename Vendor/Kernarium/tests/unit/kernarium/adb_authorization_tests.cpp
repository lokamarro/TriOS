// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumAdbAuthorization.hpp"
#include "require.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace {

using kernarium::adb::authorizationValue;
using kernarium::adb::forwardListensOnLoopbackOnly;
using uml::testing::require;
using uml::testing::requireThrows;

/* The forward grammar is the SLiRP provider's: the host address defaults to
 * loopback, and anything that is not provably loopback is not. */
void onlyALoopbackForwardListensOnLoopbackOnly()
{
    for (const char* loopback : {"tcp:127.0.0.1:5567-5555", "5567-5555",
             "tcp:5567-5555", "udp:127.1.2.3:5567-5555",
             "127.0.0.1:5567-10.0.2.15:5555", "tcp:5567-0.0.0.0:5555"})
        require(forwardListensOnLoopbackOnly(loopback), loopback);
    for (const char* beyond : {"tcp:0.0.0.0:5567-5555",
             "tcp:203.0.113.9:5567-5555", "tcp:198.51.100.7:5567-5555",
             "tcp:128.0.0.1:5567-5555", "tcp:localhost:5567-5555",
             "tcp:[::1]:5567-5555", "tcp::5567-5555", "5567", "", "tcp:",
             "tcp:127.0.0.1:5567"})
        require(!forwardListensOnLoopbackOnly(beyond), beyond);
}

/* Authorization stays on unless a launch says "0", and a launch that says
 * anything else is told so rather than guessed at. */
void authorizationStaysOnUnlessTheLaunchTurnsItOff()
{
    for (const char* request : {static_cast<const char*>(nullptr), "", "1"}) {
        require(authorizationValue(request, nullptr) == "1",
            "an unset or '1' request keeps authorization on");
        require(authorizationValue(request, "tcp:0.0.0.0:5567-5555") == "1",
            "authorization on needs no loopback forward");
    }
    for (const char* request : {"2", "off", "true", " 0", "00", "-1"})
        requireThrows<std::invalid_argument>(
            [&] { (void)authorizationValue(request, nullptr); },
            "an unknown request was accepted");
    require(kernarium::adb::requestVariable == "KERNARIUM_ANDROID_ADB_AUTH",
        "the launch setting's name");
}

/* Off gives whatever reaches the forward an unauthenticated shell (root on a
 * userdebug image), so it is allowed only where no network can reach the
 * forward: no forward, or a loopback one. */
void offIsAllowedOnlyWhereNoNetworkReachesTheForward()
{
    require(authorizationValue("0", nullptr) == "0", "no forward");
    require(authorizationValue("0", "") == "0", "an empty forward");
    require(authorizationValue("0", "tcp:127.0.0.1:5567-5555") == "0",
        "a loopback forward");
    require(authorizationValue("0", "5567-5555") == "0",
        "the default host address is loopback");
    for (const char* forward : {"tcp:0.0.0.0:5567-5555",
             "tcp:203.0.113.9:5567-5555", "tcp:198.51.100.7:5567-5555",
             "5567", "tcp:not-an-address:5567-5555"})
        requireThrows<std::invalid_argument>(
            [&] { (void)authorizationValue("0", forward); },
            "authorization went off behind a forward beyond loopback");
}

} // namespace

int main()
{
    onlyALoopbackForwardListensOnLoopbackOnly();
    authorizationStaysOnUnlessTheLaunchTurnsItOff();
    offIsAllowedOnlyWhereNoNetworkReachesTheForward();
    std::puts("PASS: ADB authorization goes off only for an automation launch "
              "on a loopback forward");
    return EXIT_SUCCESS;
}
