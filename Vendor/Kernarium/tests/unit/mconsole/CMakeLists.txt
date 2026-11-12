# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_guest_console_tests guest_console_tests.cpp)
target_link_libraries(uml_guest_console_tests PRIVATE UML::DeviceGuestConsole)
# A suspend with no reply at the one ten-second deadline is aborted with a wake.
# The request's socket stays open for its final reply, which decides a refusal
# (the guest runs) from silence or a "suspended" that followed the wake
# (unconfirmed, woken at the resume); a busy retry shares the deadline.
uml_add_unit_test(NAME guest-console-suspend-timeout
    COMMAND uml_guest_console_tests suspend-timeout
    LABELS console)
uml_add_unit_test(NAME guest-console-suspend-silent
    COMMAND uml_guest_console_tests suspend-silent
    LABELS console)
uml_add_unit_test(NAME guest-console-suspend-suspended-after-wake
    COMMAND uml_guest_console_tests suspend-suspended-after-wake
    LABELS console)
uml_add_unit_test(NAME guest-console-suspend-retry-deadline
    COMMAND uml_guest_console_tests suspend-retry-deadline
    LABELS console)
# A guest that reports itself busy is asked again for two seconds; any other
# refusal, or one that stays busy, leaves it running with no stop.
uml_add_unit_test(NAME guest-console-suspend-failure
    COMMAND uml_guest_console_tests suspend-failure
    LABELS console)
uml_add_unit_test(NAME guest-console-location
    COMMAND uml_guest_console_tests location
    LABELS console)
uml_add_unit_test(NAME guest-console-run
    COMMAND uml_guest_console_tests run
    LABELS console)
uml_add_unit_test(NAME guest-console-wake
    COMMAND uml_guest_console_tests wake
    LABELS console)
uml_add_unit_test(NAME guest-console-writeback
    COMMAND uml_guest_console_tests writeback
    LABELS console)
uml_add_unit_test(NAME guest-console-trim
    COMMAND uml_guest_console_tests trim
    LABELS console)
# The guest's memory figures: the strict reply grammar, a request sent under
# the caller's lock and waited for without it, and the guests that refuse,
# garble or never answer it.
uml_add_unit_test(NAME guest-console-memory
    COMMAND uml_guest_console_tests memory
    LABELS console)
