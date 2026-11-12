# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

include(UmlAppleSdk)

# _uml_import_static_dependency(<name> <archive> ALIAS <alias>
#     [INCLUDES <dir>...] [DEFINITIONS <definition>...] [LINKS <item>...])
# Imports <archive> as uml_third_party_<name> and names it <alias>.
function(_uml_import_static_dependency name archive)
    set(options)
    set(one_value ALIAS)
    set(multi_value INCLUDES DEFINITIONS LINKS)
    cmake_parse_arguments(ARG "${options}" "${one_value}" "${multi_value}"
        ${ARGN})

    set(_target "uml_third_party_${name}")
    add_library(${_target} STATIC IMPORTED GLOBAL)
    add_library(${ARG_ALIAS} ALIAS ${_target})
    set_target_properties(${_target} PROPERTIES
        IMPORTED_LOCATION "${archive}")
    if(ARG_INCLUDES)
        set_target_properties(${_target} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${ARG_INCLUDES}")
    endif()
    if(ARG_DEFINITIONS)
        set_target_properties(${_target} PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS "${ARG_DEFINITIONS}")
    endif()
    if(ARG_LINKS)
        set_target_properties(${_target} PROPERTIES
            INTERFACE_LINK_LIBRARIES "${ARG_LINKS}")
    endif()
endfunction()

function(uml_define_third_party_targets prefix)
    uml_find_sdk_library(_uml_sdk_math m)
    uml_find_sdk_library(_uml_sdk_resolver resolv)
    foreach(_framework IN ITEMS Foundation CoreFoundation AppKit Carbon)
        uml_find_sdk_framework(_uml_sdk_${_framework} ${_framework})
    endforeach()

    set(_include "${prefix}/include")
    set(_lib "${prefix}/lib")

    _uml_import_static_dependency(zlib "${_lib}/libz.a"
        ALIAS UML::ThirdPartyZlib
        INCLUDES "${_include}")

    _uml_import_static_dependency(pcre2 "${_lib}/libpcre2-8.a"
        ALIAS UML::ThirdPartyPcre2
        INCLUDES "${_include}"
        DEFINITIONS PCRE2_STATIC
        LINKS Threads::Threads)

    _uml_import_static_dependency(charset "${_lib}/libcharset.a"
        ALIAS UML::ThirdPartyCharset
        INCLUDES "${_include}")

    _uml_import_static_dependency(iconv "${_lib}/libiconv.a"
        ALIAS UML::ThirdPartyIconv
        INCLUDES "${_include}"
        LINKS UML::ThirdPartyCharset)

    _uml_import_static_dependency(intl "${_lib}/libintl.a"
        ALIAS UML::ThirdPartyIntl
        INCLUDES "${_include}"
        DEFINITIONS G_INTL_STATIC_COMPILATION)

    _uml_import_static_dependency(glib "${_lib}/libglib-2.0.a"
        ALIAS UML::ThirdPartyGlib
        INCLUDES
            "${_include}/glib-2.0"
            "${_lib}/glib-2.0/include"
        LINKS
            UML::ThirdPartyPcre2
            UML::ThirdPartyIntl
            UML::ThirdPartyIconv
            Threads::Threads
            "${_uml_sdk_math}"
            "${_uml_sdk_Foundation}"
            "${_uml_sdk_CoreFoundation}"
            "${_uml_sdk_AppKit}"
            "${_uml_sdk_Carbon}")

    _uml_import_static_dependency(slirp "${_lib}/libslirp.a"
        ALIAS UML::ThirdPartySlirp
        INCLUDES "${_include}/slirp"
        LINKS UML::ThirdPartyGlib "${_uml_sdk_resolver}")

    _uml_import_static_dependency(vnc_server "${_lib}/libvncserver.a"
        ALIAS UML::ThirdPartyVncServer
        INCLUDES "${_include}"
        LINKS UML::ThirdPartyZlib Threads::Threads)

    _uml_import_static_dependency(vnc_client "${_lib}/libvncclient.a"
        ALIAS UML::ThirdPartyVncClient
        INCLUDES "${_include}"
        LINKS UML::ThirdPartyZlib Threads::Threads)

    _uml_import_static_dependency(softfloat "${_lib}/libsoftfloat_3e.a"
        ALIAS UML::ThirdPartySoftFloat
        INCLUDES "${_include}"
        DEFINITIONS
            FEXCORE_PRESERVE_ALL_ATTR=
            FEXCORE_HAS_PRESERVE_ALL_ATTR=0
            SOFTFLOAT_FAST_INT64=1)

    _uml_import_static_dependency(cephes_128bit
        "${_lib}/libcephes_128bit.a"
        ALIAS UML::ThirdPartyCephes128
        INCLUDES "${_include}"
        LINKS UML::ThirdPartySoftFloat)

    _uml_import_static_dependency(fmt "${_lib}/libfmt.a"
        ALIAS UML::ThirdPartyFmt
        INCLUDES "${_include}")

    _uml_import_static_dependency(xxhash "${_lib}/libxxhash.a"
        ALIAS UML::ThirdPartyXxHash
        INCLUDES "${_include}")

    # FEXCore for the macOS-only x86-64 JIT. Its public headers change layout
    # with these definitions, which the prefix compiled it with. The embedder
    # installs FEXCore's executable-memory hooks; libJemallocDummy supplies
    # FEXCore's allocator entry points over the C library's.
    _uml_import_static_dependency(fexcore "${_lib}/libFEXCore.a"
        ALIAS UML::ThirdPartyFEXCore
        INCLUDES "${_include}"
        DEFINITIONS
            ARCHITECTURE_arm64=1
            FEX_DISABLE_TELEMETRY=1
        LINKS
            "${_lib}/libFEXCore_Base.a"
            "${_lib}/libJemallocDummy.a"
            UML::ThirdPartyFmt
            UML::ThirdPartyXxHash
            UML::ThirdPartyCephes128)
endfunction()
