// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumAdbChannel.hpp"
#include "named_tests.hpp"
#include "require.hpp"

namespace {

using namespace kernarium::adb;
using uml::testing::require;

void acceptsAKeyOfTheBase64Alphabet()
{
    require(validPublicKey("A"));
    require(validPublicKey("QUFBQQ=="));
    require(validPublicKey("abcXYZ019+/="));
    /* adb's own 2048-bit key: 524 bytes, 700 characters. */
    require(validPublicKey(std::string(700, 'A')));
    require(validPublicKey(std::string(1024, '/')));
}

void refusesAnythingElse()
{
    require(!validPublicKey(""));
    require(!validPublicKey(std::string(1025, 'A')));
    for (const char* key : {
             "AA AA", "AA\nAA", "AA,AA", "AA\tAA", "AA\"AA", "AA-AA", "AA_AA",
             "AA=BB comment", " AA", "AA ", "a:b", "\xC3\xA9"})
        require(!validPublicKey(key), key);
    require(!validPublicKey(std::string_view("AA\0AA", 5)));
}

void turnsTcpOffOnlyWhereNothingForwardsIt()
{
    require(tcpDisabled(nullptr));
    require(tcpDisabled(""));
    require(!tcpDisabled("tcp:127.0.0.1:5555-5555"));
    require(!tcpDisabled("5555-5555"));
}

void theCommandLineIsTheArgumentsAfterTheProgramNameJoinedBySpaces()
{
    require(kernelCommandLineLength({}) == 0);
    require(kernelCommandLineLength({"uml"}) == 0);
    require(kernelCommandLineLength({"uml", "mem=512M"}) == 8);
    require(kernelCommandLineLength({"uml", "a", "bc", "def"}) == 1 + 1 + 2 + 1 + 3);
}

void aCommandLineMustLeaveRoomForTheTerminator()
{
    require(fitsKernelCommandLine(0));
    require(fitsKernelCommandLine(4095));
    require(!fitsKernelCommandLine(4096));
    require(!fitsKernelCommandLine(4097));
    const auto message = commandLineTooLong(5000);
    require(message.find("5000") != std::string::npos);
    require(message.find("4095") != std::string::npos);
}

void theArgumentPrefixesEndInAnEqualsSign()
{
    require(keyPrefix.ends_with('='));
    require(tcpPrefix.ends_with('='));
    require(keyPrefix.starts_with("androidboot.kernarium."));
    require(tcpPrefix.starts_with("androidboot.kernarium."));
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"accepts-a-key-of-the-base64-alphabet", acceptsAKeyOfTheBase64Alphabet},
        {"refuses-anything-else", refusesAnythingElse},
        {"turns-tcp-off-only-where-nothing-forwards-it",
         turnsTcpOffOnlyWhereNothingForwardsIt},
        {"the-command-line-is-the-arguments-after-the-program-name-joined-by-spaces",
         theCommandLineIsTheArgumentsAfterTheProgramNameJoinedBySpaces},
        {"a-command-line-must-leave-room-for-the-terminator",
         aCommandLineMustLeaveRoomForTheTerminator},
        {"the-argument-prefixes-end-in-an-equals-sign",
         theArgumentPrefixesEndInAnEqualsSign},
    };
    return uml::testing::runNamedTests(tests);
}
