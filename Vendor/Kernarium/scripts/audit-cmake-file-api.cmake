# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED BUILD_ROOT)
    message(FATAL_ERROR "BUILD_ROOT is required")
endif()
cmake_path(ABSOLUTE_PATH BUILD_ROOT NORMALIZE OUTPUT_VARIABLE BUILD_ROOT)

set(REPLY_ROOT "${BUILD_ROOT}/.cmake/api/v1/reply")
set(ARCHITECTURE_MANIFEST "${BUILD_ROOT}/uml-architecture-targets.tsv")
file(GLOB INDEX_FILES LIST_DIRECTORIES false "${REPLY_ROOT}/index-*.json")
if(NOT INDEX_FILES)
    message(FATAL_ERROR
        "CMake File API reply is missing below ${REPLY_ROOT}; reconfigure first")
endif()
list(SORT INDEX_FILES)
list(GET INDEX_FILES -1 INDEX_FILE)
file(READ "${INDEX_FILE}" INDEX_JSON)

string(JSON CODEMODEL_FILE ERROR_VARIABLE CODEMODEL_ERROR
    GET "${INDEX_JSON}" reply codemodel-v2 jsonFile)
if(CODEMODEL_ERROR)
    message(FATAL_ERROR "CMake File API codemodel reply is unavailable: ${CODEMODEL_ERROR}")
endif()
file(READ "${REPLY_ROOT}/${CODEMODEL_FILE}" CODEMODEL_JSON)
string(JSON CONFIGURATION_COUNT LENGTH "${CODEMODEL_JSON}" configurations)
if(CONFIGURATION_COUNT LESS 1)
    message(FATAL_ERROR "CMake File API codemodel has no configurations")
endif()
string(JSON SOURCE_ROOT GET "${CODEMODEL_JSON}" paths source)

string(JSON CACHE_FILE ERROR_VARIABLE CACHE_ERROR
    GET "${INDEX_JSON}" reply cache-v2 jsonFile)
if(CACHE_ERROR)
    message(FATAL_ERROR
        "CMake File API cache reply is unavailable: ${CACHE_ERROR}; "
        "reconfigure first")
endif()
file(READ "${REPLY_ROOT}/${CACHE_FILE}" CACHE_JSON)
# A tree that names no platform is held to the iOS rules.
set(APPLE_PLATFORM "unset")
set(GPU_SOFTWARE_RENDERER_CACHE "")
string(JSON CACHE_ENTRY_COUNT LENGTH "${CACHE_JSON}" entries)
foreach(INDEX RANGE 0 ${CACHE_ENTRY_COUNT})
    if(INDEX EQUAL CACHE_ENTRY_COUNT)
        break()
    endif()
    string(JSON CACHE_ENTRY_NAME GET "${CACHE_JSON}" entries ${INDEX} name)
    if(CACHE_ENTRY_NAME STREQUAL "UML_APPLE_PLATFORM")
        string(JSON APPLE_PLATFORM GET "${CACHE_JSON}" entries ${INDEX} value)
    elseif(CACHE_ENTRY_NAME STREQUAL "UML_GPU_SOFTWARE_RENDERER")
        string(JSON GPU_SOFTWARE_RENDERER_CACHE
            GET "${CACHE_JSON}" entries ${INDEX} value)
    endif()
endforeach()

if(NOT EXISTS "${ARCHITECTURE_MANIFEST}")
    message(FATAL_ERROR
        "Architecture target manifest is missing: ${ARCHITECTURE_MANIFEST}")
endif()
# uml_write_architecture_manifest() writes one row per target from the
# declarations uml_declare_architecture_target() has validated, so each row's
# layer and engine kind are trusted once it has its five fields.
file(STRINGS "${ARCHITECTURE_MANIFEST}" ARCHITECTURE_ROWS)
set(ARCHITECTURE_TARGETS)
set(ARCHITECTURE_LAYERS)
set(ARCHITECTURE_KINDS)
set(ARCHITECTURE_ENGINE_KINDS)
set(ARCHITECTURE_DECLARED_DEPENDENCIES)
foreach(ROW IN LISTS ARCHITECTURE_ROWS)
    if(ROW MATCHES "^#" OR ROW STREQUAL "")
        continue()
    endif()
    string(REPLACE "\t" ";" FIELDS "${ROW}")
    list(LENGTH FIELDS FIELD_COUNT)
    if(NOT FIELD_COUNT EQUAL 5)
        message(FATAL_ERROR "Malformed architecture manifest row: ${ROW}")
    endif()
    list(POP_FRONT FIELDS TARGET LAYER KIND ENGINE_KIND DEPENDENCIES)
    list(APPEND ARCHITECTURE_TARGETS "${TARGET}")
    list(APPEND ARCHITECTURE_LAYERS "${LAYER}")
    list(APPEND ARCHITECTURE_KINDS "${KIND}")
    list(APPEND ARCHITECTURE_ENGINE_KINDS "${ENGINE_KIND}")
    list(APPEND ARCHITECTURE_DECLARED_DEPENDENCIES "${DEPENDENCIES}")
endforeach()

function(uml_architecture_layer TARGET OUTPUT)
    list(FIND ARCHITECTURE_TARGETS "${TARGET}" TARGET_INDEX)
    if(TARGET_INDEX EQUAL -1)
        set(${OUTPUT} "" PARENT_SCOPE)
        return()
    endif()
    list(GET ARCHITECTURE_LAYERS ${TARGET_INDEX} TARGET_LAYER)
    set(${OUTPUT} "${TARGET_LAYER}" PARENT_SCOPE)
endfunction()

# The layer direction of AGENTS.md: the layers each layer may link besides
# itself. API and platform link nothing else.
set(UML_LAYER_EDGES_memory platform)
set(UML_LAYER_EDGES_cpu memory platform)
set(UML_LAYER_EDGES_device memory platform)
set(UML_LAYER_EDGES_runtime cpu memory device platform)
set(UML_LAYER_EDGES_emulator api runtime device)
set(UML_LAYER_EDGES_app api emulator device)

function(uml_architecture_edge_allowed FROM TO OUTPUT)
    if(FROM STREQUAL TO OR TO IN_LIST UML_LAYER_EDGES_${FROM})
        set(${OUTPUT} TRUE PARENT_SCOPE)
    else()
        set(${OUTPUT} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(uml_architecture_engine_kind TARGET OUTPUT)
    list(FIND ARCHITECTURE_TARGETS "${TARGET}" ENGINE_INDEX)
    if(NOT ENGINE_INDEX EQUAL -1)
        list(GET ARCHITECTURE_ENGINE_KINDS ${ENGINE_INDEX} ENGINE_KIND)
        # "-" marks a target outside the CPU layer.
        if(NOT ENGINE_KIND STREQUAL "-")
            set(${OUTPUT} "${ENGINE_KIND}" PARENT_SCOPE)
            return()
        endif()
    endif()
    set(${OUTPUT} "" PARENT_SCOPE)
endfunction()

function(uml_architecture_engine_edge_allowed FROM_TARGET TO_TARGET OUTPUT)
    uml_architecture_engine_kind("${FROM_TARGET}" FROM_ENGINE_KIND)
    uml_architecture_engine_kind("${TO_TARGET}" TO_ENGINE_KIND)
    if(NOT FROM_ENGINE_KIND OR NOT TO_ENGINE_KIND OR
       FROM_ENGINE_KIND STREQUAL "mixed")
        set(${OUTPUT} TRUE PARENT_SCOPE)
        return()
    endif()
    if(FROM_ENGINE_KIND STREQUAL "shared")
        set(ALLOWED_KIND "shared")
    else()
        set(ALLOWED_KIND "^(shared|${FROM_ENGINE_KIND})$")
    endif()
    if(TO_ENGINE_KIND MATCHES "${ALLOWED_KIND}")
        set(${OUTPUT} TRUE PARENT_SCOPE)
    else()
        set(${OUTPUT} FALSE PARENT_SCOPE)
    endif()
endfunction()

string(JSON TARGET_COUNT LENGTH "${CODEMODEL_JSON}" configurations 0 targets)
set(TARGET_IDS)
set(TARGET_NAMES)
set(TARGET_FILES)
foreach(INDEX RANGE 0 ${TARGET_COUNT})
    if(INDEX EQUAL TARGET_COUNT)
        break()
    endif()
    string(JSON TARGET_ID GET "${CODEMODEL_JSON}" configurations 0 targets ${INDEX} id)
    string(JSON TARGET_NAME GET "${CODEMODEL_JSON}" configurations 0 targets ${INDEX} name)
    string(JSON TARGET_FILE GET "${CODEMODEL_JSON}" configurations 0 targets ${INDEX} jsonFile)
    list(APPEND TARGET_IDS "${TARGET_ID}")
    list(APPEND TARGET_NAMES "${TARGET_NAME}")
    list(APPEND TARGET_FILES "${TARGET_FILE}")
endforeach()

# Depth-first search over the declared dependency lists in the global
# properties UML_DECLARED_AUDIT_DEPS_<target>; reaching a target that is still
# active closes a cycle.
function(uml_audit_cycle_visit NODE PATH)
    get_property(STATE GLOBAL PROPERTY "UML_DECLARED_AUDIT_STATE_${NODE}")
    set(NODE_PATH ${PATH} "${NODE}")
    if(STATE STREQUAL "active")
        string(REPLACE ";" " -> " CYCLE_TEXT "${NODE_PATH}")
        message(FATAL_ERROR
            "Declared architecture target dependency cycle: ${CYCLE_TEXT}")
    endif()
    if(STATE STREQUAL "done")
        return()
    endif()
    set_property(GLOBAL PROPERTY "UML_DECLARED_AUDIT_STATE_${NODE}" active)
    get_property(DEPENDENCIES GLOBAL PROPERTY "UML_DECLARED_AUDIT_DEPS_${NODE}")
    foreach(DEPENDENCY IN LISTS DEPENDENCIES)
        uml_audit_cycle_visit("${DEPENDENCY}" "${NODE_PATH}")
    endforeach()
    set_property(GLOBAL PROPERTY "UML_DECLARED_AUDIT_STATE_${NODE}" done)
endfunction()

# Completeness in the other direction, every Darwin target declared, is
# enforced at configure time by uml_write_architecture_manifest(), which walks
# every configured target of every type.

# Audit every declared first-party edge, including pure interface contracts,
# which CMake's codemodel omits. The manifest is emitted from the configured
# targets' LINK_LIBRARIES properties, not handwritten data.
foreach(TARGET_NAME TARGET_LAYER TARGET_KIND DEPENDENCY_TEXT IN ZIP_LISTS
        ARCHITECTURE_TARGETS ARCHITECTURE_LAYERS ARCHITECTURE_KINDS
        ARCHITECTURE_DECLARED_DEPENDENCIES)
    if(TARGET_KIND STREQUAL "codemodel")
        list(FIND TARGET_NAMES "${TARGET_NAME}" TARGET_INDEX)
        if(TARGET_INDEX EQUAL -1)
            message(FATAL_ERROR
                "Declared architecture target is absent from the CMake File API: ${TARGET_NAME}")
        endif()
    endif()
    set(DEPENDENCY_NAMES)
    if(NOT DEPENDENCY_TEXT STREQUAL "-")
        string(REPLACE "," ";" DECLARED_DEPENDENCIES "${DEPENDENCY_TEXT}")
        foreach(DEPENDENCY IN LISTS DECLARED_DEPENDENCIES)
            if(NOT DEPENDENCY MATCHES "^link@([A-Za-z0-9_.+-]+)$")
                message(FATAL_ERROR
                    "Malformed architecture dependency for ${TARGET_NAME}: ${DEPENDENCY}")
            endif()
            set(DEPENDENCY_NAME "${CMAKE_MATCH_1}")
            uml_architecture_layer("${DEPENDENCY_NAME}" DEPENDENCY_LAYER)
            uml_architecture_edge_allowed(
                "${TARGET_LAYER}" "${DEPENDENCY_LAYER}" EDGE_ALLOWED)
            if(NOT EDGE_ALLOWED)
                message(FATAL_ERROR
                    "Forbidden declared link architecture edge: "
                    "${TARGET_NAME} (${TARGET_LAYER}) -> "
                    "${DEPENDENCY_NAME} (${DEPENDENCY_LAYER})")
            endif()
            list(APPEND DEPENDENCY_NAMES "${DEPENDENCY_NAME}")
        endforeach()
    endif()
    set_property(GLOBAL PROPERTY "UML_DECLARED_AUDIT_DEPS_${TARGET_NAME}"
        "${DEPENDENCY_NAMES}")
endforeach()

# Search the declared graph for cycles. It carries the interface libraries
# the codemodel omits, and CMake has already refused any other cycle or, for
# static libraries, ordered it into a chain in the codemodel.
foreach(TARGET_NAME IN LISTS ARCHITECTURE_TARGETS)
    uml_audit_cycle_visit("${TARGET_NAME}" "")
endforeach()

# An engine links its own kind or shared code, shared code links only shared
# code, and a neutral interface carrier must not conceal an engine-to-engine
# dependency. Check everything reachable from every declared engine owner,
# direct dependencies included, after the layer and cycle audits have
# established a well-formed graph. File API dependencies provide the same
# protection for concrete configured targets; this traversal additionally
# covers pure interface chains omitted from the codemodel.
function(uml_audit_engine_reachability ROOT NODE PATH)
    get_property(VISITED GLOBAL PROPERTY
        "UML_ENGINE_REACHABILITY_${ROOT}_${NODE}")
    if(VISITED)
        return()
    endif()
    set_property(GLOBAL PROPERTY
        "UML_ENGINE_REACHABILITY_${ROOT}_${NODE}" TRUE)

    uml_architecture_engine_kind("${NODE}" NODE_ENGINE_KIND)
    uml_architecture_engine_edge_allowed(
        "${ROOT}" "${NODE}" REACHABILITY_ALLOWED)
    if(NODE_ENGINE_KIND AND NOT REACHABILITY_ALLOWED)
        if(PATH)
            set(EDGE_PATH "${PATH};${NODE}")
        else()
            set(EDGE_PATH "${NODE}")
        endif()
        string(REPLACE ";" " -> " EDGE_TEXT "${EDGE_PATH}")
        message(FATAL_ERROR
            "Forbidden cross-engine transitive dependency edge: ${EDGE_TEXT}")
    endif()

    get_property(DEPENDENCIES GLOBAL PROPERTY
        "UML_DECLARED_AUDIT_DEPS_${NODE}")
    if(PATH)
        set(NEXT_PATH "${PATH};${NODE}")
    else()
        set(NEXT_PATH "${NODE}")
    endif()
    foreach(DEPENDENCY IN LISTS DEPENDENCIES)
        uml_audit_engine_reachability(
            "${ROOT}" "${DEPENDENCY}" "${NEXT_PATH}")
    endforeach()
endfunction()

foreach(TARGET_NAME IN LISTS ARCHITECTURE_TARGETS)
    uml_architecture_engine_kind("${TARGET_NAME}" TARGET_ENGINE_KIND)
    if(TARGET_ENGINE_KIND)
        uml_audit_engine_reachability(
            "${TARGET_NAME}" "${TARGET_NAME}" "")
    endif()
endforeach()

# The x86-64 JIT embeds FEXCore and is a macOS product only. Outside a macOS
# configuration no target may be the engine, compile its sources or the
# runtime seam, compile a source that includes the engine, or link FEXCore.
# The source-tree audit lets nothing but the engine and the seam include the
# engine's headers, so these rules keep it out of every iOS product.
function(uml_audit_macos_only_x86_jit TARGET_NAME TARGET_JSON)
    set(PREFIX
        "macOS-only x86-64 JIT in a UML_APPLE_PLATFORM=${APPLE_PLATFORM} configuration:")
    if(TARGET_NAME STREQUAL "uml_x86_64_jit")
        message(FATAL_ERROR "${PREFIX} target ${TARGET_NAME}")
    endif()
    string(JSON SOURCE_COUNT ERROR_VARIABLE SOURCE_ERROR
        LENGTH "${TARGET_JSON}" sources)
    if(SOURCE_ERROR)
        set(SOURCE_COUNT 0)
    endif()
    foreach(SOURCE_INDEX RANGE 0 ${SOURCE_COUNT})
        if(SOURCE_INDEX EQUAL SOURCE_COUNT)
            break()
        endif()
        string(JSON COMPILE_GROUP ERROR_VARIABLE COMPILE_GROUP_ERROR
            GET "${TARGET_JSON}" sources ${SOURCE_INDEX} compileGroupIndex)
        if(COMPILE_GROUP_ERROR)
            continue()
        endif()
        string(JSON SOURCE_PATH GET "${TARGET_JSON}"
            sources ${SOURCE_INDEX} path)
        cmake_path(ABSOLUTE_PATH SOURCE_PATH BASE_DIRECTORY "${SOURCE_ROOT}"
            NORMALIZE OUTPUT_VARIABLE SOURCE_FILE)
        cmake_path(IS_PREFIX SOURCE_ROOT "${SOURCE_FILE}" NORMALIZE
            SOURCE_IN_TREE)
        if(SOURCE_IN_TREE)
            file(RELATIVE_PATH SOURCE_PATH "${SOURCE_ROOT}" "${SOURCE_FILE}")
            if(SOURCE_PATH MATCHES
               "^darwin/(cpu/x86_64/jit/|runtime/x86_jit_runtime\\.cpp$)")
                message(FATAL_ERROR
                    "${PREFIX} ${TARGET_NAME} compiles ${SOURCE_PATH}")
            endif()
        endif()
        if(NOT EXISTS "${SOURCE_FILE}")
            continue()
        endif()
        file(READ "${SOURCE_FILE}" SOURCE_TEXT)
        string(REGEX MATCH
            "(^|\n)[ \t]*#[ \t]*(include|import)[ \t]*[<\"][^>\"\n]*cpu/x86_64/jit/"
            JIT_INCLUDE "${SOURCE_TEXT}")
        if(JIT_INCLUDE)
            message(FATAL_ERROR
                "${PREFIX} ${TARGET_NAME} compiles ${SOURCE_PATH}, which "
                "includes cpu/x86_64/jit/")
        endif()
    endforeach()
    # Only executables and shared libraries carry link fragments; a static
    # slice is covered by the source rules and the packaged-product check.
    string(JSON FRAGMENT_COUNT ERROR_VARIABLE FRAGMENT_ERROR
        LENGTH "${TARGET_JSON}" link commandFragments)
    if(FRAGMENT_ERROR)
        return()
    endif()
    foreach(FRAGMENT_INDEX RANGE 0 ${FRAGMENT_COUNT})
        if(FRAGMENT_INDEX EQUAL FRAGMENT_COUNT)
            break()
        endif()
        string(JSON FRAGMENT GET "${TARGET_JSON}"
            link commandFragments ${FRAGMENT_INDEX} fragment)
        if(FRAGMENT MATCHES "FEXCore")
            message(FATAL_ERROR
                "${PREFIX} ${TARGET_NAME} links FEXCore: ${FRAGMENT}")
        endif()
    endforeach()
endfunction()

# The software GPU renderer is the iOS Simulator's alone: the Simulator has no
# Metal 4 device for KosmicKrisp, so its product renders on the CPU, and no
# other product may fall back to that. Outside an ios-simulator configuration
# the build may not select it (UML_GPU_SOFTWARE_RENDERER), no target may compile
# with it, and no target may link a library of a software GPU prefix, which is
# the prefix that carries the Simulator's platform contract.
set(GPU_SOFTWARE_RENDERER_CONTRACT
    "share/uml-dependencies/ios-simulator-gpu-software-arm64.tsv")
if(NOT APPLE_PLATFORM STREQUAL "ios-simulator" AND
   GPU_SOFTWARE_RENDERER_CACHE)
    message(FATAL_ERROR
        "Simulator-only software GPU renderer in a "
        "UML_APPLE_PLATFORM=${APPLE_PLATFORM} configuration: cache "
        "UML_GPU_SOFTWARE_RENDERER is ${GPU_SOFTWARE_RENDERER_CACHE}")
endif()
function(uml_audit_simulator_only_software_renderer TARGET_NAME TARGET_JSON)
    set(PREFIX
        "Simulator-only software GPU renderer in a UML_APPLE_PLATFORM=${APPLE_PLATFORM} configuration:")
    string(JSON GROUP_COUNT ERROR_VARIABLE GROUP_ERROR
        LENGTH "${TARGET_JSON}" compileGroups)
    if(GROUP_ERROR)
        set(GROUP_COUNT 0)
    endif()
    foreach(GROUP_INDEX RANGE 0 ${GROUP_COUNT})
        if(GROUP_INDEX EQUAL GROUP_COUNT)
            break()
        endif()
        string(JSON DEFINE_COUNT ERROR_VARIABLE DEFINE_ERROR
            LENGTH "${TARGET_JSON}" compileGroups ${GROUP_INDEX} defines)
        if(DEFINE_ERROR)
            continue()
        endif()
        foreach(DEFINE_INDEX RANGE 0 ${DEFINE_COUNT})
            if(DEFINE_INDEX EQUAL DEFINE_COUNT)
                break()
            endif()
            string(JSON DEFINE GET "${TARGET_JSON}"
                compileGroups ${GROUP_INDEX} defines ${DEFINE_INDEX} define)
            # A bare definition means 1; the generator expression writes 0
            # or 1.
            if(DEFINE MATCHES "^UML_GPU_SOFTWARE_RENDERER(=(.*))?$")
                set(DEFINE_VALUE "${CMAKE_MATCH_2}")
                if(NOT DEFINE_VALUE STREQUAL "0")
                    message(FATAL_ERROR
                        "${PREFIX} ${TARGET_NAME} compiles with ${DEFINE}")
                endif()
            endif()
        endforeach()
    endforeach()
    string(JSON FRAGMENT_COUNT ERROR_VARIABLE FRAGMENT_ERROR
        LENGTH "${TARGET_JSON}" link commandFragments)
    if(FRAGMENT_ERROR)
        return()
    endif()
    foreach(FRAGMENT_INDEX RANGE 0 ${FRAGMENT_COUNT})
        if(FRAGMENT_INDEX EQUAL FRAGMENT_COUNT)
            break()
        endif()
        string(JSON FRAGMENT GET "${TARGET_JSON}"
            link commandFragments ${FRAGMENT_INDEX} fragment)
        # A prefix library is <prefix>/lib/libX.a or
        # <prefix>/Frameworks/X.framework/X.
        if(FRAGMENT MATCHES
           "^(.+)/(lib/[^/]+|Frameworks/[^/]+\\.framework/[^/]+)$")
            set(FRAGMENT_PREFIX "${CMAKE_MATCH_1}")
            if(EXISTS "${FRAGMENT_PREFIX}/${GPU_SOFTWARE_RENDERER_CONTRACT}")
                message(FATAL_ERROR
                    "${PREFIX} ${TARGET_NAME} links ${FRAGMENT}")
            endif()
        endif()
    endforeach()
endfunction()

foreach(INDEX RANGE 0 ${TARGET_COUNT})
    if(INDEX EQUAL TARGET_COUNT)
        break()
    endif()
    list(GET TARGET_FILES ${INDEX} TARGET_FILE)
    file(READ "${REPLY_ROOT}/${TARGET_FILE}" TARGET_JSON)
    if(NOT APPLE_PLATFORM STREQUAL "macos")
        list(GET TARGET_NAMES ${INDEX} AUDITED_TARGET_NAME)
        uml_audit_macos_only_x86_jit("${AUDITED_TARGET_NAME}" "${TARGET_JSON}")
    endif()
    if(NOT APPLE_PLATFORM STREQUAL "ios-simulator")
        list(GET TARGET_NAMES ${INDEX} AUDITED_TARGET_NAME)
        uml_audit_simulator_only_software_renderer(
            "${AUDITED_TARGET_NAME}" "${TARGET_JSON}")
    endif()
    string(JSON DEPENDENCY_COUNT ERROR_VARIABLE DEPENDENCY_ERROR
        LENGTH "${TARGET_JSON}" dependencies)
    if(NOT DEPENDENCY_ERROR)
        foreach(DEPENDENCY_INDEX RANGE 0 ${DEPENDENCY_COUNT})
            if(DEPENDENCY_INDEX EQUAL DEPENDENCY_COUNT)
                break()
            endif()
            string(JSON DEPENDENCY_ID GET "${TARGET_JSON}"
                dependencies ${DEPENDENCY_INDEX} id)
            list(FIND TARGET_IDS "${DEPENDENCY_ID}" DEPENDENCY_TARGET_INDEX)
            if(NOT DEPENDENCY_TARGET_INDEX EQUAL -1)
                list(GET TARGET_NAMES ${INDEX} DEPENDENT_TARGET_NAME)
                list(GET TARGET_NAMES ${DEPENDENCY_TARGET_INDEX}
                    DEPENDENCY_TARGET_NAME)
                uml_architecture_engine_edge_allowed(
                    "${DEPENDENT_TARGET_NAME}" "${DEPENDENCY_TARGET_NAME}"
                    CODEMODEL_ENGINE_EDGE_ALLOWED)
                if(NOT CODEMODEL_ENGINE_EDGE_ALLOWED)
                    message(FATAL_ERROR
                        "Forbidden cross-engine configured dependency edge: "
                        "${DEPENDENT_TARGET_NAME} -> ${DEPENDENCY_TARGET_NAME}")
                endif()
                # File API dependencies include transitive static-link
                # archives. They remain authoritative for engine isolation:
                # interface carriers must not hide a concrete engine
                # dependency. The declared edges above keep the layer
                # direction without mistaking archive propagation for a
                # source-level architecture dependency.
            endif()
        endforeach()
    endif()
endforeach()

list(LENGTH ARCHITECTURE_TARGETS ARCHITECTURE_TARGET_TOTAL)
message(STATUS
    "CMake File API architecture audit passed: ${TARGET_COUNT} targets, "
    "${ARCHITECTURE_TARGET_TOTAL} layered targets, no forbidden edges or "
    "cycles, platform ${APPLE_PLATFORM}")
