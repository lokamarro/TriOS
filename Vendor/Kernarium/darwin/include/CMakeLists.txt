# SPDX-License-Identifier: GPL-2.0-or-later

# The host copies of the host/Linux bridge declarations and the host-only
# guest contracts beside them. kernel/common/abi/sync.sh writes the bridge
# copies; see kernel/common/abi/README.md.
add_library(uml_bridge_abi INTERFACE)
add_library(UML::BridgeAbi ALIAS uml_bridge_abi)
uml_declare_architecture_target(uml_bridge_abi platform)
target_include_directories(uml_bridge_abi INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR})
