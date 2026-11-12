// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumAutomationJob.hpp"
#include "require.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using kernarium::automation::JobAtStart;
using kernarium::automation::parseJobAtStart;
using uml::testing::require;

/* The launch asks for the job with 1 and for nothing with 0 or by leaving the
 * variable out. */
void test_the_driver_asks_with_one()
{
    require(parseJobAtStart("1") == JobAtStart::on, "1 asks for the job");
    require(parseJobAtStart("0") == JobAtStart::off, "0 asks for none");
    require(parseJobAtStart(nullptr) == JobAtStart::off,
        "an unset variable asks for none");
}

/* Anything else is the driver's mistake. It is reported and acts as no
 * request, so a misspelt value cannot start a job a test did not mean. */
void test_anything_else_is_a_mistake()
{
    for (const char* value :
        {"", "2", "01", "10", " 1", "1 ", "true", "yes", "on", "-1"})
        require(parseJobAtStart(value) == JobAtStart::invalid, value);
}

/* The name a driver sets is part of the contract. */
void test_the_name_is_the_documented_one()
{
    require(std::strcmp(kernarium::automation::jobAtStartVariable,
                "KERNARIUM_BACKGROUND_JOB_AT_START") == 0,
        "the variable's name");
    require(std::strcmp(kernarium::automation::holdDelayVariable,
                "KERNARIUM_BACKGROUND_HOLD_DELAY_MS") == 0,
        "the hold delay variable's name");
}

/* An unset variable and zero ask for no delay, and a whole number of
 * milliseconds asks for that many. */
void test_the_hold_delay_is_a_whole_number_of_milliseconds()
{
    using kernarium::automation::parseHoldDelayMs;
    require(parseHoldDelayMs(nullptr).valid &&
            parseHoldDelayMs(nullptr).milliseconds == 0,
        "an unset variable asks for no delay");
    require(parseHoldDelayMs("0").valid && parseHoldDelayMs("0").milliseconds == 0,
        "0 asks for no delay");
    require(parseHoldDelayMs("500").milliseconds == 500, "500 is 500");
    require(parseHoldDelayMs("0750").milliseconds == 750, "leading zeros are digits");
    require(parseHoldDelayMs("30000").valid &&
            parseHoldDelayMs("30000").milliseconds == 30000,
        "the maximum is allowed");
}

/* Anything else is the driver's mistake. It is reported and acts as no delay,
 * so a misspelt value cannot move the hold a measurement placed. */
void test_anything_else_is_not_a_delay()
{
    using kernarium::automation::parseHoldDelayMs;
    for (const char* value : {"", "-1", "1.5", "1e3", " 5", "5 ", "ms", "30001",
             "99999", "100000", "000000"}) {
        require(!parseHoldDelayMs(value).valid, value);
        require(parseHoldDelayMs(value).milliseconds == 0, value);
    }
}

} // namespace

int main()
{
    test_the_driver_asks_with_one();
    test_anything_else_is_a_mistake();
    test_the_name_is_the_documented_one();
    test_the_hold_delay_is_a_whole_number_of_milliseconds();
    test_anything_else_is_not_a_delay();
    std::puts("automation job tests passed");
    return EXIT_SUCCESS;
}
