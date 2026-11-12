# SPDX-License-Identifier: GPL-2.0-or-later

if(UML_BUILD_NETWORK_PROVIDER)
    # Each probe drives the provider's test variant from its own executable.
    set(_uml_slirp_probe_targets
        uml_slirp_network_tests
        uml_slirp_forward_tests
        uml_slirp_forward_backlog_tests
        uml_slirp_reset_peer_tests
        uml_slirp_tcp_window_tests
        uml_slirp_tcp_liveness_tests
        uml_slirp_connect_failure_tests
        uml_slirp_icmp_tests
        uml_slirp_transmit_backpressure_tests
        uml_slirp_dns_tests)
    set(_uml_slirp_probe_sources
        slirp_arp_probe.cpp
        slirp_forward_probe.cpp
        slirp_forward_backlog_probe.cpp
        slirp_reset_peer_probe.cpp
        slirp_tcp_window_probe.cpp
        slirp_tcp_liveness_probe.cpp
        slirp_connect_failure_probe.cpp
        slirp_icmp_probe.cpp
        slirp_transmit_backpressure_probe.cpp
        slirp_dns_probe.cpp)
    set(_uml_slirp_probe_tests
        slirp-network-arp
        slirp-network-forward-specification
        slirp-network-forward-backlog
        slirp-network-reset-peer
        slirp-network-tcp-window
        slirp-network-tcp-liveness
        slirp-network-connect-failure
        slirp-network-icmp
        slirp-network-transmit-backpressure
        slirp-network-dns)
    foreach(_uml_slirp_target _uml_slirp_source _uml_slirp_test IN ZIP_LISTS
            _uml_slirp_probe_targets
            _uml_slirp_probe_sources
            _uml_slirp_probe_tests)
        add_executable(${_uml_slirp_target}
            ${PROJECT_SOURCE_DIR}/tests/tooling/network/${_uml_slirp_source})
        target_link_libraries(${_uml_slirp_target} PRIVATE
            uml_network_provider_test_variant
            UML::ThirdPartySlirp)
        target_include_directories(${_uml_slirp_target} PRIVATE
            ${PROJECT_SOURCE_DIR}/darwin/device/network/slirp)

        uml_add_tooling_test(
            NAME ${_uml_slirp_test}
            COMMAND ${_uml_slirp_target}
            LABELS network
            TIMEOUT 30)
    endforeach()
endif()
