# SPDX-License-Identifier: GPL-2.0-or-later

if(TARGET uml_network_dns_rules)
    add_executable(uml_dns_rules_tests dns_rules_tests.cpp)
    target_include_directories(uml_dns_rules_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp)
    target_link_libraries(uml_dns_rules_tests PRIVATE UML::NetworkDnsRules)
    uml_add_unit_test(NAME network-dns-rules
        COMMAND uml_dns_rules_tests
        LABELS network;slirp)
endif()
