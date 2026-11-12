// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumGuestTimeZone.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <string>

namespace {

using namespace kernarium::timezone;
using uml::testing::require;

void acceptsTheNamesOfTheIanaDatabase()
{
    for (const char* name : {
             "UTC", "GMT", "Asia/Tokyo", "Europe/London", "EST5EDT",
             "GB-Eire", "W-SU", "Etc/GMT+5", "Etc/GMT-14", "GMT+0800",
             "America/Port-au-Prince", "Africa/Dar_es_Salaam",
             "America/Argentina/Buenos_Aires", "America/Indiana/Tell_City",
             "Asia/Ho_Chi_Minh", "Antarctica/DumontDUrville", "a", "A0"})
        require(validIdentifier(name), name);
    /* The longest name of the database is 32 characters; the limit is 64. */
    require(validIdentifier(std::string(64, 'A')));
    require(validIdentifier("America/Argentina/ComodRivadavia"));
}

void refusesWhatCouldBeAnotherKernelArgument()
{
    for (const char* name : {
             "Asia/Tokyo x", " Asia/Tokyo", "Asia/Tokyo ", "Asia\tTokyo",
             "Asia/Tokyo\n", "Asia/\"Tokyo\"", "Asia/Tokyo,UTC",
             "Asia/Tokyo:UTC", "Asia/Tokyo=1", "Asia\\Tokyo", "Asia/To'kyo",
             "a b"})
        require(!validIdentifier(name), name);
    require(!validIdentifier(std::string_view("Asia\0Tokyo", 10)));
}

void refusesWhatIsNotAName()
{
    require(!validIdentifier(""));
    require(!validIdentifier(std::string(65, 'A')));
    for (const char* name : {
             "/", "/Asia/Tokyo", "Asia/Tokyo/", "Asia//Tokyo", "//",
             "../etc/passwd", "Asia/../Tokyo", "Asia/.", ".", "Asia.Tokyo",
             "_Asia/Tokyo", "Asia/_Tokyo", "-Asia", "+5", "Asia/-Tokyo",
             "Asia/+Tokyo", "Z\xC3\xBCrich", "Europe/Z\xC3\xBCrich"})
        require(!validIdentifier(name), name);
}

void thePrefixIsALauncherArgument()
{
    require(prefix.ends_with('='));
    require(prefix.starts_with("androidboot.kernarium."));
    require(prefix == "androidboot.kernarium.timezone=");
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"accepts-the-names-of-the-iana-database",
         acceptsTheNamesOfTheIanaDatabase},
        {"refuses-what-could-be-another-kernel-argument",
         refusesWhatCouldBeAnotherKernelArgument},
        {"refuses-what-is-not-a-name", refusesWhatIsNotAName},
        {"the-prefix-is-a-launcher-argument", thePrefixIsALauncherArgument},
    };
    return uml::testing::runNamedTests(tests);
}
