# SPDX-License-Identifier: GPL-2.0-or-later
add_executable(uml_delayed_answer_device_tests delayed_answer_device_tests.cpp)
target_link_libraries(uml_delayed_answer_device_tests PRIVATE
    UML::DelayedAnswerDevice)
uml_add_unit_test(NAME delayed-answer-device
    COMMAND uml_delayed_answer_device_tests
    LABELS vhost-user)
