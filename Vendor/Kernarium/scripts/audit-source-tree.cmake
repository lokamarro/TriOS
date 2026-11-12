# SPDX-License-Identifier: GPL-2.0-or-later
# Static source ownership and layering audit.  This intentionally examines
# assembly and implementation includes as well as ordinary C++ headers.
if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

# A darwin/ source that includes a header existing only under tests/ compiles
# only when a test target adds that directory to its include path.
function(uml_audit_test_only_includes ROOT OUT_VIOLATIONS)
    file(GLOB_RECURSE TEST_HEADERS LIST_DIRECTORIES false
        "${ROOT}/tests/*.h" "${ROOT}/tests/*.hpp")
    set(DARWIN_GLOBS)
    foreach(EXTENSION IN ITEMS c cc cpp cxx m mm h hh hpp hxx ipp inl inc S)
        list(APPEND DARWIN_GLOBS "${ROOT}/darwin/*.${EXTENSION}")
    endforeach()
    file(GLOB_RECURSE DARWIN_FILES LIST_DIRECTORIES false ${DARWIN_GLOBS})
    set(TEST_ONLY_NAMES)
    foreach(PATH IN LISTS TEST_HEADERS)
        get_filename_component(HEADER_NAME "${PATH}" NAME)
        list(APPEND TEST_ONLY_NAMES "${HEADER_NAME}")
    endforeach()
    foreach(PATH IN LISTS DARWIN_FILES)
        get_filename_component(HEADER_NAME "${PATH}" NAME)
        list(REMOVE_ITEM TEST_ONLY_NAMES "${HEADER_NAME}")
    endforeach()
    set(FOUND)
    foreach(PATH IN LISTS DARWIN_FILES)
        file(STRINGS "${PATH}" DIRECTIVES
            REGEX "^[ \t]*#[ \t]*include[ \t]*\"[^\"]+\"")
        foreach(DIRECTIVE IN LISTS DIRECTIVES)
            string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\".*$"
                "\\1" INCLUDE_NAME "${DIRECTIVE}")
            if(INCLUDE_NAME IN_LIST TEST_ONLY_NAMES)
                list(APPEND FOUND
                    "${PATH}: includes ${INCLUDE_NAME}, which exists only under tests/")
            endif()
        endforeach()
    endforeach()
    set(${OUT_VIOLATIONS} "${FOUND}" PARENT_SCOPE)
endfunction()

# C and C++ sources include neither implementation files nor the verified .inc
# contracts that assembly uses, and a CPU header creates no anonymous
# per-translation-unit namespace. The gadget composer is the one C++ file that
# includes implementation fragments: its .ipp decode families stay in one
# compiler-visible translation unit without making .ipp a general convention.
function(uml_audit_implementation_fragments ROOT OUT_VIOLATIONS)
    set(COMPOSER "${ROOT}/darwin/cpu/arm64/gadgets/program_builder.cpp")
    set(CXX_GLOBS)
    foreach(TREE IN ITEMS Andarium darwin GuestHost Kernarium tests)
        foreach(EXTENSION IN ITEMS cpp cc cxx h hh hpp ipp m mm)
            list(APPEND CXX_GLOBS "${ROOT}/${TREE}/*.${EXTENSION}")
        endforeach()
    endforeach()
    file(GLOB_RECURSE CXX_FILES LIST_DIRECTORIES false ${CXX_GLOBS})
    set(FOUND)
    foreach(PATH IN LISTS CXX_FILES)
        file(READ "${PATH}" CONTENT)
        file(RELATIVE_PATH SOURCE_PATH "${ROOT}" "${PATH}")
        if(NOT PATH STREQUAL COMPOSER AND CONTENT MATCHES
           "(^|\n)[ \t]*#[ \t]*include[ \t]+[<\"][^\">]+\\.ipp[>\"]")
            list(APPEND FOUND
                "${PATH}: only program_builder.cpp may include .ipp fragments")
        endif()
        if(CONTENT MATCHES
           "(^|\n)[ \t]*#[ \t]*include[ \t]+[^\n]*(\\.(cpp|cc|cxx|inc)|_impl\\.hpp)[\">]")
            list(APPEND FOUND
                "${PATH}: C/C++ source includes an implementation fragment")
        endif()
        if(SOURCE_PATH MATCHES "^darwin/cpu/.*\\.(h|hh|hpp|hxx)$" AND
           CONTENT MATCHES "(^|\n)[ \t]*namespace[ \t]*\\{")
            list(APPEND FOUND
                "${PATH}: CPU header creates anonymous per-translation-unit ownership")
        endif()
    endforeach()
    set(${OUT_VIOLATIONS} "${FOUND}" PARENT_SCOPE)
endfunction()

# Guest domains and performance runners may share process/console mechanics
# only through tests/integration/support. Importing another suite's entry
# point, or a guest launcher (tools/boot_*.py, tools/rootfs/*/boot.py),
# couples its command-line policy and teardown behavior into an unrelated
# test.
function(uml_audit_guest_runner_imports ROOT OUT_VIOLATIONS)
    set(_violations)
    file(GLOB_RECURSE _runners LIST_DIRECTORIES false
        "${ROOT}/tests/integration/*.py"
        "${ROOT}/tests/perf/*.py")
    foreach(_path IN LISTS _runners)
        file(READ "${_path}" _content)
        if(_content MATCHES
           "(^|\n)[ \t]*(from|import)[ \t]+((tests\.)?integration\.)?(run_guest_(ltp|smp|jit|network|gpu)|boot(_[A-Za-z0-9_]+)?[ \t\r\n.,(])")
            list(APPEND _violations
                "${_path}: guest suites must share helpers through tests/integration/support")
        endif()
    endforeach()
    set(${OUT_VIOLATIONS} "${_violations}" PARENT_SCOPE)
endfunction()

# First-party CMake files and Xcode projects (Kernarium's project.pbxproj and
# the JSON project.xcproj of Andarium, or of Kernarium once converted) are each
# read once for three rules.
#
# A darwin/ implementation source is compiled only by the CMake owner beneath
# darwin/ that lists it. Any other target reaches it through a product that
# owner builds: a CMake test links the owner's target or test variant, and an
# Xcode target links a framework or archive packaged from the owner's build.
# Naming the source from another CMake file or from an Xcode project compiles
# a second copy under other flags.
#
# All first-party CTest registrations pass through the suite helpers in
# cmake/UmlTesting.cmake, so suite labels, fixture policy, and aggregate
# binary dependencies cannot be bypassed by a raw add_test() in another
# directory. CMake command names are case-insensitive, so the source text is
# normalized before matching. A command starts its line, and CMake refuses a
# newline or comment before its parenthesis, so a commented-out registration
# never matches.
#
# A compile definition, or a -D compile option, that names a source or build
# directory puts this checkout's absolute path into a binary, which then fails
# wherever the checkout is absent, as in a relocated test bundle. A test takes
# a data path as a registration argument instead. The definitions are scanned
# without CMake lists, which would split a command at its semicolons and
# brackets.
function(uml_audit_build_files ROOT OUT_VIOLATIONS)
    set(_source "darwin/[A-Za-z0-9_./-]+\\.(cpp|cxx|cc|mm|m|S|s)")
    set(_violations)
    set(_cmake_globs)
    foreach(_scope IN ITEMS Andarium android cmake darwin dependencies
            GuestHost kernel Kernarium scripts tests tools)
        list(APPEND _cmake_globs
            "${ROOT}/${_scope}/*CMakeLists.txt" "${ROOT}/${_scope}/*.cmake")
    endforeach()
    file(GLOB_RECURSE _cmake_files LIST_DIRECTORIES false ${_cmake_globs})
    file(GLOB _root_files LIST_DIRECTORIES false "${ROOT}/CMakeLists.txt")
    foreach(_path IN LISTS _root_files _cmake_files)
        file(RELATIVE_PATH _relative "${ROOT}" "${_path}")
        file(READ "${_path}" _text)
        if(NOT _relative MATCHES "^darwin/")
            string(REGEX REPLACE "#[^\n]*" "" _code "${_text}")
            string(REGEX MATCHALL "(_SOURCE_DIR}/|\\.\\./)${_source}" _named
                "${_code}")
            foreach(_name IN LISTS _named)
                string(REGEX REPLACE "^(_SOURCE_DIR}/|\\.\\./)" "" _name "${_name}")
                list(APPEND _violations
                    "${_path}: compiles ${_name}, which only its darwin/ owner may build")
            endforeach()
        endif()
        string(TOLOWER "${_text}" _normalized_text)
        if(_relative MATCHES
           "^(CMakeLists\\.txt$|(Andarium|android|cmake|darwin|dependencies|GuestHost|kernel|Kernarium|tests)/)"
           AND NOT _relative STREQUAL "cmake/UmlTesting.cmake" AND
           _normalized_text MATCHES "(^|[\r\n])[ \t]*add_test[ \t\r]*\\(")
            list(APPEND _violations
                "${_path}: raw add_test bypasses the centralized suite helpers")
        endif()
        string(REGEX REPLACE "#[^\n]*" "" _definition_code "${_normalized_text}")
        string(REGEX REPLACE "[][;]" " " _definition_code "${_definition_code}")
        string(REGEX MATCHALL
            "(^|[\r\n])[ \t]*(target_compile_definitions|add_compile_definitions|add_definitions|target_compile_options|add_compile_options)[ \t\r]*\\([^)]*"
            _definition_commands "${_definition_code}")
        foreach(_command IN LISTS _definition_commands)
            set(_definition "")
            if(_command MATCHES "_options[ \t\r]*\\(")
                set(_definition "-d[a-z0-9_]+=[^ \t\r\n]*")
            endif()
            if(_command MATCHES
               "${_definition}(\\$\\{[a-z0-9_]*_(source|binary|list)_dir\\}|\\$<target_(file|bundle|linker_file)_dir:)")
                list(APPEND _violations
                    "${_path}: a compile definition names a source or build directory")
            endif()
        endforeach()
    endforeach()
    file(GLOB_RECURSE _projects LIST_DIRECTORIES false
        "${ROOT}/Kernarium/*.pbxproj")
    foreach(_path IN LISTS _projects)
        file(READ "${_path}" _text)
        string(REGEX MATCHALL "[ \t{]path = \"?([^\";\n]*/)?${_source}"
            _references "${_text}")
        foreach(_reference IN LISTS _references)
            string(REGEX REPLACE "^.*path = \"?" "" _name "${_reference}")
            list(APPEND _violations
                "${_path}: an Xcode target compiles ${_name}, which only its darwin/ owner may build")
        endforeach()
    endforeach()
    # A JSON project names a file in a "path" entry. It is JSON5, so the key
    # is quoted with either quote or bare, and the value takes either quote.
    # A source must end at the closing quote, so a longer extension such as
    # .swift is not the .s of an assembly file. GuestHost holds shared source
    # that the apps compile, not a project.
    set(_json_path
        "[ \t\r\n{,][\"']?path[\"']?[ \t\r\n]*:[ \t\r\n]*[\"']([^\"'\n]*/)?${_source}[\"']")
    file(GLOB_RECURSE _json_projects LIST_DIRECTORIES false
        "${ROOT}/Andarium/*.xcproj" "${ROOT}/Kernarium/*.xcproj")
    foreach(_path IN LISTS _json_projects)
        file(READ "${_path}" _text)
        string(REGEX MATCHALL "${_json_path}" _references "${_text}")
        foreach(_reference IN LISTS _references)
            string(REGEX REPLACE "^[^:]*:[ \t\r\n]*[\"']" "" _name "${_reference}")
            string(REGEX REPLACE "[\"']$" "" _name "${_name}")
            list(APPEND _violations
                "${_path}: an Xcode target compiles ${_name}, which only its darwin/ owner may build")
        endforeach()
    endforeach()
    set(${OUT_VIOLATIONS} "${_violations}" PARENT_SCOPE)
endfunction()

# A shell searches an exported CDPATH before the working directory for a cd
# operand that does not start with / or a . component, and prints the
# directory it finds there, so a decoy replaces the intended directory and its
# path lands in captured output. A first-party shell script therefore writes
# every cd as CDPATH= cd -- unless its operand is a literal absolute path. A
# variable operand takes the form even when its value is absolute, so the rule
# needs neither data flow nor an exemption. Comments are ignored, and the text
# is scanned without CMake lists, which would split at its semicolons and
# brackets.
function(uml_audit_shell_cd ROOT OUT_VIOLATIONS)
    set(_globs)
    foreach(_scope IN ITEMS Andarium android cmake darwin dependencies
            GuestHost kernel Kernarium scripts tests tools)
        list(APPEND _globs "${ROOT}/${_scope}/*.sh")
    endforeach()
    file(GLOB_RECURSE _scripts LIST_DIRECTORIES false ${_globs})
    # A cd command word, its operand and the character that ends it.
    set(_cd "[\n \t;&|(){}!`'\"]cd([ \t][^\n;&|)]*)?[\n;&|)]")
    set(_violations)
    foreach(_path IN LISTS _scripts)
        file(READ "${_path}" _text)
        string(REPLACE "CDPATH= cd -- " "CDPATH=@cd-- " _code "\n${_text}\n")
        string(REGEX REPLACE "\n[ \t]*#[^\n]*" "\n" _code "${_code}")
        string(REGEX REPLACE "[ \t]#[^\n]*" "" _code "${_code}")
        set(_offset 0)
        set(_rest "${_code}")
        while(TRUE)
            if(NOT _rest MATCHES "${_cd}")
                break()
            endif()
            set(_match "${CMAKE_MATCH_0}")
            string(FIND "${_rest}" "${_match}" _at)
            string(LENGTH "${_match}" _length)
            if(NOT _match MATCHES "^.cd[ \t]+(--[ \t]+)?[\"']?/")
                math(EXPR _end "${_offset} + ${_at} + 1")
                string(SUBSTRING "${_code}" 0 ${_end} _before)
                string(REGEX MATCHALL "\n" _newlines "${_before}")
                list(LENGTH _newlines _line)
                math(EXPR _command_length "${_length} - 2")
                string(SUBSTRING "${_match}" 1 ${_command_length} _command)
                string(STRIP "${_command}" _command)
                list(APPEND _violations
                    "${_path}:${_line}: ${_command} needs CDPATH= cd -- unless it names a literal absolute directory")
            endif()
            # The character that ended this cd may begin the next one.
            math(EXPR _next "${_at} + ${_length} - 1")
            math(EXPR _offset "${_offset} + ${_next}")
            string(SUBSTRING "${_rest}" ${_next} -1 _rest)
        endwhile()
    endforeach()
    set(${OUT_VIOLATIONS} "${_violations}" PARENT_SCOPE)
endfunction()

# Darwin reserves x18 as the platform register and clears it whenever it
# interrupts a thread, so a value kept there is lost at a random preemption.
# No first-party host source (darwin/, Kernarium/, Andarium/ and the GuestHost/
# source the apps share) names it outside a comment: not assembly or an
# assembly contract, not inline assembly, and not a generator that writes
# either. What a JIT composes at run time is for the emitted-code unit tests.
function(uml_audit_platform_register ROOT OUT_VIOLATIONS)
    set(_globs)
    foreach(_tree IN ITEMS Andarium darwin GuestHost Kernarium)
        foreach(_extension IN ITEMS
                S s inc c cc cpp cxx h hh hpp ipp inl m mm py)
            list(APPEND _globs "${ROOT}/${_tree}/*.${_extension}")
        endforeach()
    endforeach()
    file(GLOB_RECURSE _sources LIST_DIRECTORIES false ${_globs})
    set(_violations)
    foreach(_path IN LISTS _sources)
        # A Python line comment starts with #; elsewhere # is a directive.
        set(_comment "^[ \t]*(/\\*|\\*|//)")
        if(_path MATCHES "[.]py$")
            set(_comment "^[ \t]*#")
        endif()
        file(STRINGS "${_path}" _lines
            REGEX "(^|[^A-Za-z0-9_])[wx]18([^0-9]|$)")
        foreach(_line IN LISTS _lines)
            if(_line MATCHES "${_comment}")
                continue()
            endif()
            list(APPEND _violations
                "${_path}: names x18, which Darwin clears when it interrupts a thread")
            break()
        endforeach()
    endforeach()
    set(${OUT_VIOLATIONS} "${_violations}" PARENT_SCOPE)
endfunction()

set(VIOLATIONS)

file(GLOB ROOT_C_SOURCES LIST_DIRECTORIES false
    "${SOURCE_ROOT}/*.c"
    "${SOURCE_ROOT}/Makefile")
set(C_SOURCES ${ROOT_C_SOURCES})
foreach(FIRST_PARTY_SCOPE IN ITEMS
        Andarium cmake darwin dependencies GuestHost kernel Kernarium scripts
        tests tools)
    if(NOT IS_DIRECTORY "${SOURCE_ROOT}/${FIRST_PARTY_SCOPE}")
        continue()
    endif()
    file(GLOB_RECURSE SCOPED_C_SOURCES LIST_DIRECTORIES false
        "${SOURCE_ROOT}/${FIRST_PARTY_SCOPE}/*.c"
        "${SOURCE_ROOT}/${FIRST_PARTY_SCOPE}/Makefile")
    list(APPEND C_SOURCES ${SCOPED_C_SOURCES})
endforeach()
list(REMOVE_DUPLICATES C_SOURCES)
set(ALLOWED_C_SOURCES
    "${SOURCE_ROOT}/tests/consumer/apple/main.c")
foreach(PATH IN LISTS C_SOURCES)
    if(PATH IN_LIST ALLOWED_C_SOURCES)
        # The installed-package acceptance is a genuine C consumer and is
        # not compiled into the host project.
        continue()
    endif()
    list(APPEND VIOLATIONS "${PATH}: project C source is not allowed")
endforeach()

# The layers whose files each layer may not include.
set(FORBIDDEN_LAYERS_cpu "runtime|device|app")
set(FORBIDDEN_LAYERS_memory "cpu|runtime|device|app")
set(FORBIDDEN_LAYERS_device "cpu|runtime|app")
set(FORBIDDEN_LAYERS_platform "cpu")
set(LAYER_GLOBS)
foreach(EXTENSION IN ITEMS
        c cc cpp cxx m mm h hh hpp hxx ipp inl def S s asm inc)
    foreach(LAYER_DIRECTORY IN ITEMS cpu memory runtime device api app emulator platform)
        list(APPEND LAYER_GLOBS
            "${SOURCE_ROOT}/darwin/${LAYER_DIRECTORY}/*.${EXTENSION}")
    endforeach()
endforeach()
file(GLOB_RECURSE LAYER_SOURCES LIST_DIRECTORIES false ${LAYER_GLOBS})
foreach(PATH IN LISTS LAYER_SOURCES)
    file(READ "${PATH}" CONTENT)
    # Classify by the path inside the tree, so a checkout below a directory
    # named like a layer is not mistaken for one.
    file(RELATIVE_PATH SOURCE_PATH "${SOURCE_ROOT}" "${PATH}")
    set(NORMALIZED_INCLUDES)
    string(REGEX MATCHALL
        "#[ \t]*include[ \t]*[\"<][^\">]+[\">]"
        INCLUDE_DIRECTIVES "${CONTENT}")
    get_filename_component(SOURCE_DIRECTORY "${PATH}" DIRECTORY)
    foreach(INCLUDE_DIRECTIVE IN LISTS INCLUDE_DIRECTIVES)
        string(REGEX REPLACE
            "^#[ \t]*include[ \t]*[\"<]([^\">]+)[\">]$"
            "\\1" INCLUDE_NAME "${INCLUDE_DIRECTIVE}")
        set(INCLUDE_CANDIDATES
            "${SOURCE_DIRECTORY}/${INCLUDE_NAME}"
            "${SOURCE_ROOT}/darwin/${INCLUDE_NAME}"
            "${SOURCE_ROOT}/darwin/include/${INCLUDE_NAME}")
        set(NORMALIZED_INCLUDE "${INCLUDE_NAME}")
        foreach(INCLUDE_CANDIDATE IN LISTS INCLUDE_CANDIDATES)
            cmake_path(NORMAL_PATH INCLUDE_CANDIDATE)
            if(EXISTS "${INCLUDE_CANDIDATE}")
                file(RELATIVE_PATH NORMALIZED_INCLUDE
                    "${SOURCE_ROOT}" "${INCLUDE_CANDIDATE}")
                break()
            endif()
        endforeach()
        list(APPEND NORMALIZED_INCLUDES "${NORMALIZED_INCLUDE}")
    endforeach()
    string(JOIN "\n" NORMALIZED_INCLUDE_TEXT ${NORMALIZED_INCLUDES})
    string(REGEX REPLACE "^darwin/([a-z]+)/.*$" "\\1" LAYER "${SOURCE_PATH}")
    if(DEFINED FORBIDDEN_LAYERS_${LAYER})
        if(NORMALIZED_INCLUDE_TEXT MATCHES
           "(^|\n)(darwin/)?(${FORBIDDEN_LAYERS_${LAYER}})/")
            list(APPEND VIOLATIONS
                "${PATH}: ${LAYER} layer includes the ${CMAKE_MATCH_3} layer")
        endif()
    endif()
    # Engines do not include one another and shared CPU code includes neither.
    # The JITs own arm64/jit/ and x86_64/jit/, the gadget engines own
    # arm64/gadgets/ and x86_64/gadgets/, the rest of x86_64/ is what the x86
    # engines share, and the engine factory is the one source that composes
    # the arm64 engines.
    set(FOREIGN_ENGINE_INCLUDE "")
    if(SOURCE_PATH MATCHES "^darwin/cpu/arm64/jit/")
        set(FOREIGN_ENGINE_INCLUDE "cpu/(arm64/gadgets|x86_64)/")
    elseif(SOURCE_PATH MATCHES "^darwin/cpu/x86_64/jit/")
        set(FOREIGN_ENGINE_INCLUDE "cpu/(arm64/jit|arm64/gadgets|x86_64/gadgets)/")
    elseif(SOURCE_PATH MATCHES "^darwin/cpu/(arm64|x86_64)/gadgets/")
        set(FOREIGN_ENGINE_INCLUDE "cpu/(arm64|x86_64)/jit/")
    elseif(SOURCE_PATH MATCHES "^darwin/cpu/x86_64/")
        set(FOREIGN_ENGINE_INCLUDE "cpu/(arm64/jit|arm64/gadgets|x86_64/gadgets|x86_64/jit)/")
    elseif(SOURCE_PATH MATCHES "^darwin/cpu/" AND
           NOT SOURCE_PATH MATCHES "^darwin/cpu/engine_factory\\.cpp$")
        set(FOREIGN_ENGINE_INCLUDE "cpu/(arm64/jit|arm64/gadgets|x86_64)/")
    endif()
    if(FOREIGN_ENGINE_INCLUDE AND NORMALIZED_INCLUDE_TEXT MATCHES
       "(^|\n)(darwin/)?${FOREIGN_ENGINE_INCLUDE}")
        list(APPEND VIOLATIONS
            "${PATH}: CPU source includes an engine implementation it does not own")
    endif()
    # The gadget engines and shared CPU code stay data-only, so only a JIT
    # engine includes the executable-memory owner.
    if(SOURCE_PATH MATCHES "^darwin/cpu/" AND
       NOT SOURCE_PATH MATCHES "^darwin/cpu/(arm64/jit|x86_64/jit|jit_memory)/" AND
       NORMALIZED_INCLUDE_TEXT MATCHES "(^|\n)(darwin/)?cpu/jit_memory/")
        list(APPEND VIOLATIONS
            "${PATH}: only a JIT engine may include the executable-memory owner")
    endif()
    # The x86-64 JIT is macOS-only. Outside the engine, only the runtime
    # seam that the macOS build alone compiles includes it, so no source
    # an iOS product builds can reach it.
    if(NOT SOURCE_PATH MATCHES "^darwin/cpu/x86_64/jit/" AND
       NOT SOURCE_PATH MATCHES "^darwin/runtime/x86_jit_runtime\\.cpp$" AND
       NORMALIZED_INCLUDE_TEXT MATCHES "(^|\n)(darwin/)?cpu/x86_64/jit/")
        list(APPEND VIOLATIONS
            "${PATH}: only the macOS runtime seam may include the x86-64 JIT")
    endif()
    # Sandboxed iOS can execute the precompiled gadget engine without a
    # dynamic-code entitlement. Keep executable allocation and
    # publication primitives confined to the executable-memory owner;
    # merely avoiding a simultaneous W+X mapping is not sufficient because an
    # RW-to-RX transition is also dynamic code publication.
    if(SOURCE_PATH MATCHES "^darwin/cpu/" AND
       NOT SOURCE_PATH MATCHES "^darwin/cpu/jit_memory/executable_memory\\.cpp$" AND
       CONTENT MATCHES
       "(MAP_JIT|PROT_EXEC|VM_PROT_EXECUTE|mprotect[ \t]*\\(|mach_vm_protect[ \t]*\\(|vm_protect[ \t]*\\(|pthread_jit_write_protect|sys_icache_invalidate[ \t]*\\()")
        list(APPEND VIOLATIONS
            "${PATH}: only the executable-memory owner may use executable-memory or code-publication primitives")
    endif()
endforeach()

uml_audit_implementation_fragments(
    "${SOURCE_ROOT}" IMPLEMENTATION_FRAGMENT_VIOLATIONS)
list(APPEND VIOLATIONS ${IMPLEMENTATION_FRAGMENT_VIOLATIONS})

uml_audit_test_only_includes("${SOURCE_ROOT}" TEST_ONLY_INCLUDE_VIOLATIONS)
list(APPEND VIOLATIONS ${TEST_ONLY_INCLUDE_VIOLATIONS})

file(GLOB_RECURSE UNIT_SCRIPT_RUNNERS LIST_DIRECTORIES false
    "${SOURCE_ROOT}/tests/unit/*.py"
    "${SOURCE_ROOT}/tests/unit/*.sh")
foreach(PATH IN LISTS UNIT_SCRIPT_RUNNERS)
    list(APPEND VIOLATIONS
        "${PATH}: Python and shell are not allowed in the host unit-test tree")
endforeach()

uml_audit_guest_runner_imports("${SOURCE_ROOT}" GUEST_RUNNER_IMPORT_VIOLATIONS)
list(APPEND VIOLATIONS ${GUEST_RUNNER_IMPORT_VIOLATIONS})

uml_audit_build_files("${SOURCE_ROOT}" BUILD_FILE_VIOLATIONS)
list(APPEND VIOLATIONS ${BUILD_FILE_VIOLATIONS})

uml_audit_shell_cd("${SOURCE_ROOT}" SHELL_CD_VIOLATIONS)
list(APPEND VIOLATIONS ${SHELL_CD_VIOLATIONS})

uml_audit_platform_register("${SOURCE_ROOT}" PLATFORM_REGISTER_VIOLATIONS)
list(APPEND VIOLATIONS ${PLATFORM_REGISTER_VIOLATIONS})

# Verify that every implementation source is owned by the nearest CMakeLists.txt
# at or above it, using its complete module-relative path. A same-named source
# in another module is not evidence of ownership, and darwin/CMakeLists.txt
# composes subdirectories without owning leaf sources. Only trees that CMake
# builds are owned this way: Xcode compiles the sources of Andarium/ and
# GuestHost/, so they have no CMake owner.
set(OWNERSHIP_GLOBS)
foreach(OWNED_TREE IN ITEMS darwin Kernarium/Runtime)
    foreach(EXTENSION IN ITEMS cpp cc cxx m mm S s)
        list(APPEND OWNERSHIP_GLOBS "${SOURCE_ROOT}/${OWNED_TREE}/*.${EXTENSION}")
    endforeach()
endforeach()
file(GLOB_RECURSE IMPLEMENTATION_SOURCES LIST_DIRECTORIES false
    ${OWNERSHIP_GLOBS})
foreach(PATH IN LISTS IMPLEMENTATION_SOURCES)
    file(RELATIVE_PATH SOURCE_RELATIVE_PATH "${SOURCE_ROOT}" "${PATH}")
    get_filename_component(MANIFEST_DIRECTORY "${PATH}" DIRECTORY)
    while(NOT EXISTS "${MANIFEST_DIRECTORY}/CMakeLists.txt" AND
          NOT MANIFEST_DIRECTORY STREQUAL SOURCE_ROOT)
        get_filename_component(MANIFEST_DIRECTORY "${MANIFEST_DIRECTORY}"
            DIRECTORY)
    endwhile()
    set(MANIFEST "${MANIFEST_DIRECTORY}/CMakeLists.txt")
    if(MANIFEST STREQUAL "${SOURCE_ROOT}/darwin/CMakeLists.txt" OR
       MANIFEST STREQUAL "${SOURCE_ROOT}/CMakeLists.txt")
        list(APPEND VIOLATIONS
            "${PATH}: implementation source has no CMake owner below the composition file")
        continue()
    endif()
    file(RELATIVE_PATH MANIFEST_RELATIVE_PATH "${MANIFEST_DIRECTORY}" "${PATH}")
    file(READ "${MANIFEST}" MANIFEST_TEXT)
    string(REGEX REPLACE "#[^\n]*" "" MANIFEST_CODE "${MANIFEST_TEXT}")
    string(FIND "${MANIFEST_CODE}" "${MANIFEST_RELATIVE_PATH}"
        RELATIVE_OWNER_INDEX)
    string(FIND "${MANIFEST_CODE}" "${SOURCE_RELATIVE_PATH}"
        SOURCE_OWNER_INDEX)
    if(RELATIVE_OWNER_INDEX EQUAL -1 AND SOURCE_OWNER_INDEX EQUAL -1)
        list(APPEND VIOLATIONS
            "${PATH}: implementation source is not owned by its nearest CMakeLists.txt")
    endif()
endforeach()

# Validate concrete paths named by the architecture guide. Markdown links are
# resolved relative to the document; inline module paths are resolved first
# below darwin/ and then from the repository root.
set(ARCHITECTURE_DOCUMENT "${SOURCE_ROOT}/docs/architecture.md")
file(READ "${ARCHITECTURE_DOCUMENT}" ARCHITECTURE_TEXT)
file(STRINGS "${ARCHITECTURE_DOCUMENT}" ARCHITECTURE_LINES)
foreach(DOCUMENT_LINE IN LISTS ARCHITECTURE_LINES)
    if(DOCUMENT_LINE MATCHES "\\]\\(([^)]+)\\)")
        set(REFERENCE "${CMAKE_MATCH_1}")
        if(REFERENCE MATCHES "^(https?:|#)")
            continue()
        endif()
        set(CANDIDATE "${SOURCE_ROOT}/docs/${REFERENCE}")
        cmake_path(NORMAL_PATH CANDIDATE)
        if(NOT EXISTS "${CANDIDATE}")
            list(APPEND VIOLATIONS
                "${ARCHITECTURE_DOCUMENT}: broken Markdown path ${REFERENCE}")
        endif()
    endif()
endforeach()
string(REGEX MATCHALL
    "`[A-Za-z0-9_./-]+\\.(h|hpp|cpp|cmake|tsv|md|txt)`"
    ARCHITECTURE_INLINE_PATHS "${ARCHITECTURE_TEXT}")
foreach(QUOTED_PATH IN LISTS ARCHITECTURE_INLINE_PATHS)
    string(REGEX REPLACE "^`([^`]+)`$" "\\1" REFERENCE "${QUOTED_PATH}")
    set(CANDIDATES
        "${SOURCE_ROOT}/darwin/${REFERENCE}"
        "${SOURCE_ROOT}/${REFERENCE}"
        "${SOURCE_ROOT}/docs/${REFERENCE}")
    set(REFERENCE_EXISTS FALSE)
    foreach(CANDIDATE IN LISTS CANDIDATES)
        cmake_path(NORMAL_PATH CANDIDATE)
        if(EXISTS "${CANDIDATE}")
            set(REFERENCE_EXISTS TRUE)
            break()
        endif()
    endforeach()
    if(NOT REFERENCE_EXISTS)
        list(APPEND VIOLATIONS
            "${ARCHITECTURE_DOCUMENT}: stale inline path ${REFERENCE}")
    endif()
endforeach()

# Keep project documentation together. The root retains only the agent guide,
# its compatibility link, and the conventional human-facing README.
file(GLOB ROOT_MARKDOWN_DOCUMENTS LIST_DIRECTORIES false
    "${SOURCE_ROOT}/*.md")
set(ROOT_MARKDOWN_ALLOWLIST
    AGENTS.md
    CLAUDE.md
    README.md)
foreach(ROOT_MARKDOWN_DOCUMENT IN LISTS ROOT_MARKDOWN_DOCUMENTS)
    get_filename_component(ROOT_MARKDOWN_NAME
        "${ROOT_MARKDOWN_DOCUMENT}" NAME)
    if(NOT ROOT_MARKDOWN_NAME IN_LIST ROOT_MARKDOWN_ALLOWLIST)
        list(APPEND VIOLATIONS
            "${ROOT_MARKDOWN_DOCUMENT}: project documentation belongs under docs/")
    endif()
endforeach()

if(VIOLATIONS)
    list(JOIN VIOLATIONS "\n  " FORMATTED_VIOLATIONS)
    message(FATAL_ERROR
        "Host source-tree architecture audit failed:\n  ${FORMATTED_VIOLATIONS}")
endif()
