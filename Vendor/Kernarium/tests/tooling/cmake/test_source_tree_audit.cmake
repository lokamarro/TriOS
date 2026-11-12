# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED AUDIT_SCRIPT OR AUDIT_SCRIPT STREQUAL "")
    message(FATAL_ERROR "AUDIT_SCRIPT is required")
endif()
if(NOT EXISTS "${AUDIT_SCRIPT}")
    message(FATAL_ERROR "AUDIT_SCRIPT does not exist: ${AUDIT_SCRIPT}")
endif()
if(NOT DEFINED TEST_ROOT OR TEST_ROOT STREQUAL "")
    message(FATAL_ERROR "TEST_ROOT is required")
endif()

# Including the audit checks the live tree; the fixtures below check that the
# implementation-fragment rule rejects an .ipp include outside the composer,
# that a darwin/ source cannot include a header that exists only under tests/,
# that a guest runner cannot import a guest launcher, and that neither a CMake
# file outside darwin/ nor an Xcode project (Kernarium's pbxproj, or a JSON
# project.xcproj) compiles a darwin/ source.
include("${AUDIT_SCRIPT}")

file(REMOVE_RECURSE "${TEST_ROOT}")
file(WRITE "${TEST_ROOT}/darwin/cpu/arm64/gadgets/program_builder.cpp"
    "#include \"program_builder_fixture_impl.ipp\"\n")
file(WRITE "${TEST_ROOT}/darwin/cpu/arm64/gadgets/program_builder_fixture_impl.ipp"
    "// fixture\n")
uml_audit_implementation_fragments("${TEST_ROOT}" COMPOSER_VIOLATIONS)
if(COMPOSER_VIOLATIONS)
    message(FATAL_ERROR
        "source audit rejected the composer's fragment: ${COMPOSER_VIOLATIONS}")
endif()

file(WRITE "${TEST_ROOT}/tests/unit/arm64/fixture_tests.cpp"
    "#include \"program_builder_fixture_impl.ipp\"\n")
uml_audit_implementation_fragments("${TEST_ROOT}" FOREIGN_VIOLATIONS)
if(NOT FOREIGN_VIOLATIONS MATCHES "fixture_tests.cpp: only program_builder.cpp")
    message(FATAL_ERROR "source audit accepted an .ipp include outside the composer")
endif()

file(WRITE "${TEST_ROOT}/darwin/device/fixture/fixture_device.cpp"
    "#include \"fixture_hook_testing.hpp\"\n")
file(WRITE "${TEST_ROOT}/tests/unit/fixture/fixture_hook_testing.hpp"
    "// fixture\n")
uml_audit_test_only_includes("${TEST_ROOT}" TEST_ONLY_VIOLATIONS)
if(NOT TEST_ONLY_VIOLATIONS MATCHES
   "fixture_device.cpp: includes fixture_hook_testing.hpp, which exists only under tests/")
    message(FATAL_ERROR "source audit accepted a darwin/ include of a test-only header")
endif()
file(RENAME "${TEST_ROOT}/tests/unit/fixture/fixture_hook_testing.hpp"
    "${TEST_ROOT}/darwin/device/fixture/fixture_hook_testing.hpp")
uml_audit_test_only_includes("${TEST_ROOT}" BESIDE_SOURCE_VIOLATIONS)
if(BESIDE_SOURCE_VIOLATIONS)
    message(FATAL_ERROR
        "source audit rejected a hook header beside its source: ${BESIDE_SOURCE_VIOLATIONS}")
endif()

file(WRITE "${TEST_ROOT}/tests/integration/fixture/run_fixture.py"
    "import sys\nimport boot_kde_vnc  # noqa: E402\n")
uml_audit_guest_runner_imports("${TEST_ROOT}" LAUNCHER_IMPORT_VIOLATIONS)
if(NOT LAUNCHER_IMPORT_VIOLATIONS MATCHES "run_fixture.py: guest suites must share")
    message(FATAL_ERROR "source audit accepted a guest runner importing a launcher")
endif()
file(WRITE "${TEST_ROOT}/tests/integration/fixture/run_fixture.py"
    "import sys\nimport kde_session  # noqa: E402\nimport bootstrap_fixture\n")
uml_audit_guest_runner_imports("${TEST_ROOT}" SUPPORT_IMPORT_VIOLATIONS)
if(SUPPORT_IMPORT_VIOLATIONS)
    message(FATAL_ERROR
        "source audit rejected a guest runner's support import: ${SUPPORT_IMPORT_VIOLATIONS}")
endif()

# The live audit reads this file too, so the fixtures spell the darwin
# directory through a variable rather than as a literal source path.
set(FIXTURE_TREE darwin)
file(WRITE "${TEST_ROOT}/tests/unit/fixture/CMakeLists.txt"
    "add_executable(fixture_tests fixture_tests.cpp\n"
    "    \${PROJECT_SOURCE_DIR}/${FIXTURE_TREE}/device/fixture/fixture_device.cpp)\n")
file(WRITE "${TEST_ROOT}/Kernarium/Fixture.xcodeproj/project.pbxproj"
    "\t\tD1 /* fixture.mm */ = {isa = PBXFileReference; name = fixture.mm; "
    "path = ../${FIXTURE_TREE}/platform/apple/fixture.mm; sourceTree = SOURCE_ROOT; };\n")
# A JSON project is JSON5: the "path" key and its value use either quote or,
# for the key, none, and the value may follow on the next line.
file(WRITE "${TEST_ROOT}/Andarium/Fixture.xcodeproj/project.xcproj"
    "{\n"
    "  \"files\": [\n"
    "    { \"path\": \"../${FIXTURE_TREE}/platform/apple/canonical.mm\", \"target-membership\": [ \"Fixture\" ] },\n"
    "    { path: '../${FIXTURE_TREE}/platform/apple/bare.cpp' },\n"
    "    { 'path': \"../${FIXTURE_TREE}/cpu/single.S\" },\n"
    "    { \"path\": '${FIXTURE_TREE}/cpu/mixed.cxx' },\n"
    "    { \"path\":\n      \"../${FIXTURE_TREE}/device/split.cc\" },\n"
    "  ],\n"
    "}\n")
file(WRITE "${TEST_ROOT}/Kernarium/Fixture.xcodeproj/project.xcproj"
    "{ \"files\": [ { \"path\": \"../${FIXTURE_TREE}/platform/apple/converted.mm\" } ] }\n")
uml_audit_build_files("${TEST_ROOT}" FOREIGN_SOURCE_VIOLATIONS)
if(NOT FOREIGN_SOURCE_VIOLATIONS MATCHES
   "fixture/CMakeLists.txt: compiles ${FIXTURE_TREE}/device/fixture/fixture_device\\.cpp")
    message(FATAL_ERROR "source audit accepted a test CMake file compiling a darwin/ source")
endif()
if(NOT FOREIGN_SOURCE_VIOLATIONS MATCHES
   "project.pbxproj: an Xcode target compiles \\.\\./${FIXTURE_TREE}/platform/apple/fixture\\.mm")
    message(FATAL_ERROR "source audit accepted an Xcode target compiling a darwin/ source")
endif()
foreach(JSON_NAME IN ITEMS
        "\\.\\./${FIXTURE_TREE}/platform/apple/canonical\\.mm"
        "\\.\\./${FIXTURE_TREE}/platform/apple/bare\\.cpp"
        "\\.\\./${FIXTURE_TREE}/cpu/single\\.S"
        "${FIXTURE_TREE}/cpu/mixed\\.cxx"
        "\\.\\./${FIXTURE_TREE}/device/split\\.cc")
    if(NOT FOREIGN_SOURCE_VIOLATIONS MATCHES
       "Andarium/Fixture.xcodeproj/project.xcproj: an Xcode target compiles ${JSON_NAME},")
        message(FATAL_ERROR
            "source audit accepted a JSON project compiling ${JSON_NAME}:\n${FOREIGN_SOURCE_VIOLATIONS}")
    endif()
endforeach()
if(NOT FOREIGN_SOURCE_VIOLATIONS MATCHES
   "Kernarium/Fixture.xcodeproj/project.xcproj: an Xcode target compiles \\.\\./${FIXTURE_TREE}/platform/apple/converted\\.mm")
    message(FATAL_ERROR "source audit ignored a converted Kernarium JSON project")
endif()
file(WRITE "${TEST_ROOT}/tests/unit/fixture/CMakeLists.txt"
    "add_executable(fixture_tests fixture_tests.cpp)\n"
    "target_include_directories(fixture_tests PRIVATE\n"
    "    \${PROJECT_SOURCE_DIR}/${FIXTURE_TREE}/device/fixture)\n"
    "target_link_libraries(fixture_tests PRIVATE UML::FixtureDevice)\n")
file(WRITE "${TEST_ROOT}/Kernarium/Fixture.xcodeproj/project.pbxproj"
    "\t\tD2 /* Fixture.xcframework */ = {isa = PBXFileReference; "
    "name = Fixture.xcframework; "
    "path = \"../.build/kernarium-xcframeworks/Fixture.xcframework\"; "
    "sourceTree = SOURCE_ROOT; };\n")
# A JSON project may name a packaged product, a header, a source folder, a
# longer extension and another key that merely ends in "path".
file(WRITE "${TEST_ROOT}/Andarium/Fixture.xcodeproj/project.xcproj"
    "{\n"
    "  \"files\": [\n"
    "    { \"path\": \"../.build/kernarium-xcframeworks/Fixture.xcframework\" },\n"
    "    { \"path\": \"../${FIXTURE_TREE}/api/include/uml/fixture.h\" },\n"
    "    { \"path\": \"../${FIXTURE_TREE}/platform/apple/fixture.swift\" },\n"
    "    { \"path\": \"Fixture\", \"kind\": \"folder\" },\n"
    "    { \"base-path\": \"../${FIXTURE_TREE}/platform/apple/other.mm\" },\n"
    "  ],\n"
    "}\n")
file(WRITE "${TEST_ROOT}/Kernarium/Fixture.xcodeproj/project.xcproj"
    "{ path: 'Runtime/Fixture.xcframework' }\n")
uml_audit_build_files("${TEST_ROOT}" OWNER_PRODUCT_VIOLATIONS)
if(OWNER_PRODUCT_VIOLATIONS)
    message(FATAL_ERROR
        "source audit rejected targets linking the owner's product: ${OWNER_PRODUCT_VIOLATIONS}")
endif()

# The project-C-source, layer, engine-ownership, executable-memory,
# implementation-include, CPU-header, test-registration, compile-definition,
# shell cd and x18 rules read the whole tree, so the audit itself runs over a
# fixture tree that breaks each rule beside a file the rule accepts. Every
# fixture starts on its second line, so a rule cannot pass by matching only at
# the start of a file.
file(REMOVE_RECURSE "${TEST_ROOT}")
file(WRITE "${TEST_ROOT}/CMakeLists.txt" "")
file(WRITE "${TEST_ROOT}/docs/architecture.md" "")
function(uml_write_audit_fixture PATH TEXT)
    file(WRITE "${TEST_ROOT}/${PATH}" "// fixture\n${TEXT}\n")
endfunction()
set(REJECTED)
set(ACCEPTED)

# The apps and their shared source folder hold no project C source; third-party
# sources outside the first-party scopes are not this rule's concern.
uml_write_audit_fixture(Andarium/fixture/andarium_stray.c "int fixture;")
uml_write_audit_fixture(GuestHost/fixture/guesthost_stray.c "int fixture;")
uml_write_audit_fixture(external/fixture/vendored_stray.c "int fixture;")
list(APPEND REJECTED
    "andarium_stray\\.c: project C source is not allowed"
    "guesthost_stray\\.c: project C source is not allowed")
list(APPEND ACCEPTED "vendored_stray\\.c: project C source")

uml_write_audit_fixture(darwin/cpu/fixture/cpu_layer.cpp
    "#include \"runtime/fixture.hpp\"")
uml_write_audit_fixture(darwin/memory/fixture/memory_layer.cpp
    "#include \"device/fixture.hpp\"")
uml_write_audit_fixture(darwin/device/fixture/device_layer.cpp
    "#include <app/fixture.hpp>")
uml_write_audit_fixture(darwin/platform/fixture/platform_layer.hpp
    "#include \"darwin/cpu/fixture.hpp\"")
uml_write_audit_fixture(darwin/device/fixture/device_memory.cpp
    "#include \"memory/fixture.hpp\"")
uml_write_audit_fixture(darwin/runtime/fixture/runtime_device.cpp
    "#include \"device/fixture.hpp\"")
list(APPEND REJECTED
    "cpu/fixture/cpu_layer\\.cpp: [A-Za-z]+ layer includes"
    "memory/fixture/memory_layer\\.cpp: [A-Za-z]+ layer includes"
    "device/fixture/device_layer\\.cpp: [A-Za-z]+ layer includes"
    "platform/fixture/platform_layer\\.hpp: [A-Za-z]+ layer includes")
uml_write_audit_fixture(darwin/memory/fixture/tab_layer.cpp
    "#include\t\"runtime/fixture.hpp\"")
list(APPEND REJECTED "tab_layer\\.cpp: [A-Za-z]+ layer includes")
# Neither memory nor device code depends on the CPU.
uml_write_audit_fixture(darwin/memory/fixture/memory_cpu.cpp
    "#include \"cpu/fixture.hpp\"")
uml_write_audit_fixture(darwin/device/fixture/device_cpu.cpp
    "#include <darwin/cpu/fixture.hpp>")
list(APPEND REJECTED
    "memory_cpu\\.cpp: memory layer includes the cpu layer"
    "device_cpu\\.cpp: device layer includes the cpu layer")
list(APPEND ACCEPTED
    "device_memory\\.cpp: [A-Za-z]+ layer"
    "runtime_device\\.cpp: [A-Za-z]+ layer")

uml_write_audit_fixture(darwin/cpu/fixture/fragment_source.cpp
    "#include \"fixture_table.cpp\"")
uml_write_audit_fixture(tests/fixture/fragment_contract.cpp
    "#include \"fixture_contract.inc\"")
uml_write_audit_fixture(Kernarium/Runtime/fragment_impl.mm
    "#include \"runtime_impl.hpp\"")
uml_write_audit_fixture(darwin/cpu/fixture/header_include.cpp
    "#include \"fixture_contract.hpp\"")
list(APPEND REJECTED
    "fragment_source\\.cpp: C/C\\+\\+ source includes an implementation fragment"
    "fragment_contract\\.cpp: C/C\\+\\+ source includes an implementation fragment"
    "fragment_impl\\.mm: C/C\\+\\+ source includes an implementation fragment")
list(APPEND ACCEPTED "header_include\\.cpp: C/C\\+\\+ source")

# The apps and the shared host source follow the same rules, and no source
# outside the composer includes an .ipp fragment.
uml_write_audit_fixture(Andarium/fixture/andarium_fragment.mm
    "#include \"fixture_table.cpp\"")
uml_write_audit_fixture(GuestHost/fixture/guesthost_fragment.mm
    "#include \"fixture_contract.inc\"")
uml_write_audit_fixture(GuestHost/fixture/guesthost_ipp.mm
    "#include \"fixture_decode.ipp\"")
uml_write_audit_fixture(GuestHost/fixture/guesthost_header.mm
    "#include \"GuestHost.h\"")
list(APPEND REJECTED
    "andarium_fragment\\.mm: C/C\\+\\+ source includes an implementation fragment"
    "guesthost_fragment\\.mm: C/C\\+\\+ source includes an implementation fragment"
    "guesthost_ipp\\.mm: only program_builder\\.cpp may include \\.ipp fragments")
list(APPEND ACCEPTED "guesthost_header\\.mm: C/C\\+\\+ source")

uml_write_audit_fixture(darwin/cpu/fixture/anonymous.hpp
    "namespace fixture {\nnamespace {\n}\n}")
uml_write_audit_fixture(darwin/cpu/fixture/anonymous_source.cpp
    "namespace {\n}")
uml_write_audit_fixture(darwin/device/fixture/anonymous_device.hpp
    "namespace {\n}")
list(APPEND REJECTED
    "anonymous\\.hpp: CPU header creates anonymous per-translation-unit ownership")
list(APPEND ACCEPTED
    "anonymous_source\\.cpp: CPU header"
    "anonymous_device\\.hpp: CPU header")

uml_write_audit_fixture(darwin/cpu/arm64/jit/jit_fixture.cpp
    "#include \"cpu/arm64/gadgets/fixture.hpp\"")
uml_write_audit_fixture(darwin/cpu/x86_64/x86_fixture.cpp
    "#include \"cpu/arm64/jit/fixture.hpp\"")
uml_write_audit_fixture(darwin/cpu/shared_fixture.cpp
    "#include \"cpu/x86_64/fixture.hpp\"")
uml_write_audit_fixture(darwin/cpu/engine_factory.cpp
    "#include \"cpu/arm64/jit/fixture.hpp\"\n#include \"cpu/x86_64/fixture.hpp\"")
# The shared x86 code includes no x86 engine; the x86 gadget engine includes
# the shared x86 code and no JIT.
uml_write_audit_fixture(darwin/cpu/x86_64/x86_shared_fixture.cpp
    "#include \"cpu/x86_64/gadgets/fixture.hpp\"")
uml_write_audit_fixture(darwin/cpu/x86_64/gadgets/x86_gadget_fixture.cpp
    "#include \"cpu/x86_64/fixture.hpp\"")
uml_write_audit_fixture(darwin/cpu/x86_64/gadgets/x86_gadget_jit_fixture.cpp
    "#include \"cpu/arm64/jit/fixture.hpp\"")
list(APPEND REJECTED
    "jit_fixture\\.cpp: CPU source includes an engine implementation"
    "x86_fixture\\.cpp: CPU source includes an engine implementation"
    "shared_fixture\\.cpp: CPU source includes an engine implementation"
    "x86_shared_fixture\\.cpp: CPU source includes an engine implementation"
    "x86_gadget_jit_fixture\\.cpp: CPU source includes an engine implementation")
list(APPEND ACCEPTED
    "engine_factory\\.cpp: CPU source includes"
    "x86_gadget_fixture\\.cpp: CPU source includes")

# The x86-64 JIT includes the shared x86 code and neither gadget engine, no
# gadget engine or shared x86 source includes it, and outside the engine only
# the macOS runtime seam does.
uml_write_audit_fixture(darwin/cpu/x86_64/jit/x86_jit_gadget_fixture.cpp
    "#include \"cpu/x86_64/gadgets/fixture.hpp\"")
uml_write_audit_fixture(darwin/cpu/x86_64/jit/x86_jit_shared_fixture.cpp
    "#include \"cpu/x86_64/fixture.hpp\"")
uml_write_audit_fixture(darwin/cpu/x86_64/gadgets/x86_gadget_x86_jit_fixture.cpp
    "#include \"cpu/x86_64/jit/fixture.hpp\"")
uml_write_audit_fixture(darwin/cpu/x86_64/x86_shared_jit_fixture.cpp
    "#include \"cpu/x86_64/jit/fixture.hpp\"")
uml_write_audit_fixture(darwin/runtime/x86_jit_runtime.cpp
    "#include \"cpu/x86_64/jit/fixture.hpp\"")
uml_write_audit_fixture(darwin/runtime/runtime_x86_jit_fixture.cpp
    "#include \"cpu/x86_64/jit/fixture.hpp\"")
list(APPEND REJECTED
    "x86_jit_gadget_fixture\\.cpp: CPU source includes an engine implementation"
    "x86_gadget_x86_jit_fixture\\.cpp: CPU source includes an engine implementation"
    "x86_gadget_x86_jit_fixture\\.cpp: only the macOS runtime seam"
    "x86_shared_jit_fixture\\.cpp: CPU source includes an engine implementation"
    "runtime_x86_jit_fixture\\.cpp: only the macOS runtime seam")
list(APPEND ACCEPTED
    "x86_jit_shared_fixture\\.cpp: CPU source includes"
    "x86_jit_runtime\\.cpp: only the macOS runtime seam")

uml_write_audit_fixture(darwin/cpu/arm64/gadgets/gadget_owner.cpp
    "#include \"cpu/jit_memory/executable_memory.hpp\"")
uml_write_audit_fixture(darwin/cpu/x86_64/gadgets/x86_owner.cpp
    "#include \"cpu/jit_memory/executable_memory.hpp\"")
uml_write_audit_fixture(darwin/cpu/shared_owner.cpp
    "#include \"darwin/cpu/jit_memory/executable_memory.hpp\"")
uml_write_audit_fixture(darwin/cpu/arm64/jit/jit_owner.cpp
    "#include \"cpu/jit_memory/executable_memory.hpp\"")
uml_write_audit_fixture(darwin/cpu/jit_memory/owner_include.cpp
    "#include \"cpu/jit_memory/executable_memory.hpp\"")
uml_write_audit_fixture(darwin/cpu/x86_64/jit/x86_jit_owner.cpp
    "#include \"cpu/jit_memory/executable_memory.hpp\"")
list(APPEND REJECTED
    "gadget_owner\\.cpp: only a JIT engine may include"
    "x86_owner\\.cpp: only a JIT engine may include"
    "shared_owner\\.cpp: only a JIT engine may include")
list(APPEND ACCEPTED
    "jit_owner\\.cpp: only a JIT engine"
    "x86_jit_owner\\.cpp: only a JIT engine"
    "owner_include\\.cpp: only a JIT engine")

uml_write_audit_fixture(darwin/cpu/arm64/gadgets/gadget_exec.cpp
    "int publish() { return mprotect(nullptr, 0, 0); }")
uml_write_audit_fixture(darwin/cpu/fixture/cpu_exec.cpp
    "int protection = PROT_EXEC;")
uml_write_audit_fixture(darwin/cpu/arm64/jit/jit_arena.cpp
    "int publish() { return mprotect(nullptr, 0, 0); }")
uml_write_audit_fixture(darwin/cpu/jit_memory/executable_memory.hpp
    "int protection = PROT_EXEC;")
uml_write_audit_fixture(darwin/cpu/jit_memory/executable_memory.cpp
    "int publish() { return mprotect(nullptr, 0, 0); }")
uml_write_audit_fixture(darwin/memory/fixture/memory_exec.cpp
    "int protection = PROT_EXEC;")
list(APPEND REJECTED
    "gadget_exec\\.cpp: [^\n]*executable-memory"
    "cpu_exec\\.cpp: [^\n]*executable-memory"
    "jit_arena\\.cpp: [^\n]*executable-memory"
    "executable_memory\\.hpp: [^\n]*executable-memory")
list(APPEND ACCEPTED
    "executable_memory\\.cpp: [^\n]*executable-memory"
    "memory_exec\\.cpp: [^\n]*executable-memory")

uml_write_audit_fixture(tests/fixture/CMakeLists.txt
    "add_test(NAME fixture COMMAND fixture)")
uml_write_audit_fixture(tests/fixture/Suite.cmake "ADD_TEST (fixture fixture)")
# CMake reads a carriage return as a space, before the command and before its
# parenthesis alike.
uml_write_audit_fixture(tests/fixture/Carriage.cmake
    "\t\r add_test \r(NAME fixture COMMAND fixture)")
uml_write_audit_fixture(tests/fixture/Commented.cmake
    "# add_test(NAME fixture COMMAND fixture)")
uml_write_audit_fixture(cmake/UmlTesting.cmake
    "add_test(NAME fixture COMMAND fixture)")
# tools/ holds guest-side CMake projects that never join the host build.
uml_write_audit_fixture(tools/fixture/CMakeLists.txt
    "add_test(NAME fixture COMMAND fixture)")
list(APPEND REJECTED
    "fixture/CMakeLists\\.txt: raw add_test bypasses"
    "fixture/Suite\\.cmake: raw add_test bypasses"
    "fixture/Carriage\\.cmake: raw add_test bypasses")
list(APPEND ACCEPTED
    "Commented\\.cmake: raw add_test"
    "UmlTesting\\.cmake: raw add_test"
    "tools/fixture/CMakeLists\\.txt: raw add_test")

# The apps and the shared host source register no raw test, name no darwin/
# source from CMake and put no source or build directory in a definition. A
# commented-out registration there is still accepted.
uml_write_audit_fixture(Andarium/fixture/AndariumSuite.cmake
    "add_test(NAME fixture COMMAND fixture)")
uml_write_audit_fixture(GuestHost/fixture/GuestHostSuite.cmake
    "add_test(NAME fixture COMMAND fixture)")
uml_write_audit_fixture(Andarium/fixture/AndariumCommented.cmake
    "# add_test(NAME fixture COMMAND fixture)")
uml_write_audit_fixture(Andarium/fixture/AndariumOwner.cmake
    "add_library(fixture \${PROJECT_SOURCE_DIR}/${FIXTURE_TREE}/fixture/andarium_owned.cpp)")
uml_write_audit_fixture(GuestHost/fixture/GuestHostOwner.cmake
    "add_library(fixture \${PROJECT_SOURCE_DIR}/${FIXTURE_TREE}/fixture/guesthost_owned.cpp)")
uml_write_audit_fixture(Andarium/fixture/AndariumDefinition.cmake
    "target_compile_definitions(fixture PRIVATE\n    FIXTURE_DIR=\"\${PROJECT_SOURCE_DIR}/Andarium\")")
uml_write_audit_fixture(GuestHost/fixture/GuestHostDefinition.cmake
    "target_compile_options(fixture PRIVATE\n    -DFIXTURE_ROOT=\${CMAKE_CURRENT_BINARY_DIR})")
list(APPEND REJECTED
    "AndariumSuite\\.cmake: raw add_test bypasses"
    "GuestHostSuite\\.cmake: raw add_test bypasses"
    "AndariumOwner\\.cmake: compiles ${FIXTURE_TREE}/fixture/andarium_owned\\.cpp"
    "GuestHostOwner\\.cmake: compiles ${FIXTURE_TREE}/fixture/guesthost_owned\\.cpp"
    "AndariumDefinition\\.cmake: a compile definition names"
    "GuestHostDefinition\\.cmake: a compile definition names")
list(APPEND ACCEPTED "AndariumCommented\\.cmake: raw add_test")

# A darwin/ owner names its own sources.
uml_write_audit_fixture(darwin/fixture/CMakeLists.txt
    "add_library(fixture \${PROJECT_SOURCE_DIR}/${FIXTURE_TREE}/fixture/owned.cpp)")
list(APPEND ACCEPTED "darwin/fixture/CMakeLists\\.txt: compiles")

# A definition command or a -D option that names a source or build directory,
# as a variable or a generator expression, is rejected. A path passed as a
# test argument, a plain value, an option that defines nothing and a comment
# are accepted, and a later command is not part of the definition before it.
uml_write_audit_fixture(tests/fixture/SourceDefinition.cmake
    "target_compile_definitions(fixture PRIVATE\n    A=1\n    FIXTURE_DIR=\"\${PROJECT_SOURCE_DIR}/tests/support/fixture\")")
uml_write_audit_fixture(tools/fixture/BinaryDefinition.cmake
    "ADD_COMPILE_DEFINITIONS(FIXTURE_OUTPUT=\"\${CMAKE_CURRENT_BINARY_DIR}\")")
uml_write_audit_fixture(darwin/fixture/OptionDefinition.cmake
    "target_compile_options(fixture PRIVATE -Wall\n    -DFIXTURE_ROOT=\${CMAKE_CURRENT_LIST_DIR})")
uml_write_audit_fixture(kernel/fixture/TargetDefinition.cmake
    "add_definitions(-DFIXTURE_TOOL=\"\$<TARGET_FILE_DIR:fixture>\")")
list(APPEND REJECTED
    "SourceDefinition\\.cmake: a compile definition names a source or build directory"
    "BinaryDefinition\\.cmake: a compile definition names"
    "OptionDefinition\\.cmake: a compile definition names"
    "TargetDefinition\\.cmake: a compile definition names")
uml_write_audit_fixture(tests/fixture/ArgumentPath.cmake
    "uml_add_unit_test(NAME fixture COMMAND fixture\n    \${PROJECT_SOURCE_DIR}/tests/support/fixture)")
uml_write_audit_fixture(tests/fixture/ValueDefinition.cmake
    "target_compile_definitions(fixture PRIVATE\n    FIXTURE_PAGES=\${UML_GUEST_PAGE_SIZE})\ntarget_include_directories(fixture PRIVATE\n    \${PROJECT_SOURCE_DIR}/${FIXTURE_TREE})")
uml_write_audit_fixture(tests/fixture/PrefixMap.cmake
    "target_compile_options(fixture PRIVATE\n    -fmacro-prefix-map=\${PROJECT_SOURCE_DIR}=.)")
uml_write_audit_fixture(tests/fixture/CommentedDefinition.cmake
    "# target_compile_definitions(fixture PRIVATE ROOT=\"\${PROJECT_SOURCE_DIR}\")")
list(APPEND ACCEPTED
    "ArgumentPath\\.cmake: a compile definition"
    "ValueDefinition\\.cmake: a compile definition"
    "PrefixMap\\.cmake: a compile definition"
    "CommentedDefinition\\.cmake: a compile definition")

# A first-party shell script's cd clears CDPATH unless its operand is a
# literal absolute path: each report names the line and the command it found,
# including a second cd on the line of an accepted one.
uml_write_audit_fixture(tools/fixture/variable_cd.sh "cd \"\$work\"")
uml_write_audit_fixture(android/fixture/script_directory.sh
    "here=\$(cd -- \"\$(dirname -- \"\$0\")\" && pwd)")
uml_write_audit_fixture(dependencies/fixture/subshell_cd.sh
    "(cd \"\$source\" && find .)")
uml_write_audit_fixture(Kernarium/fixture/uncleared_option.sh
    "root=\$(CDPATH= cd \"\$root\" && pwd -P)")
uml_write_audit_fixture(scripts/fixture/chained_cd.sh
    "CDPATH= cd -- \"\$a\" && cd \"\$b\"")
uml_write_audit_fixture(kernel/fixture/separated_cd.sh
    "sh -c 'cd /usr/src; cd src && make'")
uml_write_audit_fixture(tests/fixture/nested_cd.sh
    "if true\nthen\n\tcd build\nfi")
uml_write_audit_fixture(cmake/fixture/bare_cd.sh "cd")
list(APPEND REJECTED
    "variable_cd\\.sh:2: cd \".work\" needs CDPATH= cd --"
    "script_directory\\.sh:2: cd -- \".\\(dirname -- \".0\" needs CDPATH= cd --"
    "subshell_cd\\.sh:2: cd \".source\" needs CDPATH= cd --"
    "uncleared_option\\.sh:2: cd \".root\" needs CDPATH= cd --"
    "chained_cd\\.sh:2: cd \".b\" needs CDPATH= cd --"
    "separated_cd\\.sh:2: cd src needs CDPATH= cd --"
    "nested_cd\\.sh:4: cd build needs CDPATH= cd --"
    "bare_cd\\.sh:2: cd needs CDPATH= cd --")
uml_write_audit_fixture(tools/fixture/cleared_cd.sh
    "root=\$(CDPATH= cd -- \"\$root\" && pwd -P)\n(CDPATH= cd -- \"\$x\" && make)")
uml_write_audit_fixture(tools/fixture/absolute_cd.sh
    "cd /usr/src/ltp\nsh -c 'cd /splash; cmake -B /build'\ncd -- \"/opt\"")
uml_write_audit_fixture(tools/fixture/commented_cd.sh
    "# cd \"\$work\" first\nmake  # then cd build")
uml_write_audit_fixture(tools/fixture/cd_words.sh
    "cdrom=1\nprintf '%s\\n' abcd cd-image --cd")
uml_write_audit_fixture(tools/fixture/cd_in_python.py
    "subprocess.run('cd build', shell=True)")
# The apps and the shared host source are first-party scopes too.
uml_write_audit_fixture(Andarium/fixture/andarium_cd.sh "cd \"\$work\"")
uml_write_audit_fixture(GuestHost/fixture/guesthost_cd.sh
    "(cd \"\$source\" && make)")
uml_write_audit_fixture(Andarium/fixture/andarium_cleared_cd.sh
    "root=\$(CDPATH= cd -- \"\$root\" && pwd -P)")
uml_write_audit_fixture(GuestHost/fixture/guesthost_absolute_cd.sh
    "cd /usr/src/ltp")
list(APPEND REJECTED
    "andarium_cd\\.sh:2: cd \".work\" needs CDPATH= cd --"
    "guesthost_cd\\.sh:2: cd \".source\" needs CDPATH= cd --")
list(APPEND ACCEPTED
    "andarium_cleared_cd\\.sh:"
    "guesthost_absolute_cd\\.sh:")
# Third-party sources live outside the first-party directories.
uml_write_audit_fixture(external/fixture/vendored.sh "cd \"\$work\"")
list(APPEND ACCEPTED
    "cleared_cd\\.sh:"
    "absolute_cd\\.sh:"
    "commented_cd\\.sh:"
    "cd_words\\.sh:"
    "cd_in_python\\.py:"
    "vendored\\.sh:")

# No host source names x18 outside a comment, in any language that can name
# a register. Guest assembly outside darwin/, Kernarium/, Andarium/ and
# GuestHost/ may use it.
uml_write_audit_fixture(darwin/cpu/fixture/setf_flags.inc
    "\tand w18, w27, #0x20000000")
uml_write_audit_fixture(darwin/cpu/fixture/entry_pair.S
    "\tstp x18, x19, [sp, #-0x80]!")
uml_write_audit_fixture(darwin/cpu/fixture/define_register.S
    "#define SCRATCH x18")
uml_write_audit_fixture(darwin/platform/fixture/read_register.cpp
    "asm volatile(\"mov %0, x18\" : \"=r\"(value));")
uml_write_audit_fixture(Kernarium/fixture/clobber.mm
    "asm volatile(\"\" ::: \"x18\");")
uml_write_audit_fixture(darwin/cpu/fixture/write_gadget.py
    "emit('mov w18, w0')")
uml_write_audit_fixture(Andarium/fixture/andarium_clobber.mm
    "asm volatile(\"\" ::: \"x18\");")
uml_write_audit_fixture(GuestHost/fixture/guesthost_register.cpp
    "asm volatile(\"mov %0, x18\" : \"=r\"(value));")
uml_write_audit_fixture(GuestHost/fixture/guesthost_generator.py
    "emit('mov w18, w0')")
list(APPEND REJECTED
    "andarium_clobber\\.mm: names x18"
    "guesthost_register\\.cpp: names x18"
    "guesthost_generator\\.py: names x18"
    "setf_flags\\.inc: names x18"
    "entry_pair\\.S: names x18"
    "define_register\\.S: names x18"
    "read_register\\.cpp: names x18"
    "clobber\\.mm: names x18"
    "write_gadget\\.py: names x18")
uml_write_audit_fixture(darwin/cpu/fixture/commented_register.S
    "/* x18 belongs to Darwin.\n * So does w18.\n */\n// x18 again")
uml_write_audit_fixture(darwin/cpu/fixture/similar_registers.S
    "\tmovi v18.16b, #0x18\n\tldr q18, [x0, #0x18]\n\tmov x1, xzr // ax18 x180")
uml_write_audit_fixture(darwin/cpu/fixture/commented_gadget.py
    "# x18 belongs to Darwin")
uml_write_audit_fixture(tests/fixture/guest_loop.S "\tmov x18, x0")
uml_write_audit_fixture(Andarium/fixture/andarium_commented.mm
    "// x18 belongs to Darwin\n/* so does w18 */")
list(APPEND ACCEPTED
    "andarium_commented\\.mm: names x18"
    "commented_register\\.S: names x18"
    "similar_registers\\.S: names x18"
    "commented_gadget\\.py: names x18"
    "guest_loop\\.S: names x18")

# Xcode compiles the apps' sources and the shared host source, so no CMake file
# owns them as it owns a darwin/ source.
uml_write_audit_fixture(Andarium/fixture/andarium_unowned.mm "int fixture;")
uml_write_audit_fixture(GuestHost/fixture/guesthost_unowned.cpp "int fixture;")
list(APPEND ACCEPTED
    "andarium_unowned\\.mm: implementation source"
    "guesthost_unowned\\.cpp: implementation source")

# The audit reads a JSON project too, and a clean one passes.
uml_write_audit_fixture(Andarium/Fixture.xcodeproj/project.xcproj
    "{ files: [ { path: '../${FIXTURE_TREE}/platform/apple/andarium_tree.mm' } ] }")
uml_write_audit_fixture(Andarium/Clean.xcodeproj/project.xcproj
    "{ \"files\": [ { \"path\": \"../.build/Fixture.xcframework\" } ] }")
list(APPEND REJECTED
    "Fixture\\.xcodeproj/project\\.xcproj: an Xcode target compiles \\.\\./${FIXTURE_TREE}/platform/apple/andarium_tree\\.mm")
list(APPEND ACCEPTED "Clean\\.xcodeproj/project\\.xcproj: an Xcode target")

execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${TEST_ROOT}" -P "${AUDIT_SCRIPT}"
    RESULT_VARIABLE AUDIT_RESULT
    OUTPUT_VARIABLE AUDIT_OUTPUT
    ERROR_VARIABLE AUDIT_OUTPUT)
if(AUDIT_RESULT EQUAL 0)
    message(FATAL_ERROR "source audit accepted the fixture tree")
endif()
foreach(EXPECTED IN LISTS REJECTED)
    if(NOT AUDIT_OUTPUT MATCHES "/${EXPECTED}")
        message(FATAL_ERROR
            "source audit did not report ${EXPECTED}:\n${AUDIT_OUTPUT}")
    endif()
endforeach()
foreach(UNEXPECTED IN LISTS ACCEPTED)
    if(AUDIT_OUTPUT MATCHES "/${UNEXPECTED}")
        message(FATAL_ERROR
            "source audit reported ${UNEXPECTED}:\n${AUDIT_OUTPUT}")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
