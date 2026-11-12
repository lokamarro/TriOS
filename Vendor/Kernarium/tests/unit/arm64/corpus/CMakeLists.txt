# SPDX-License-Identifier: GPL-2.0-or-later

include(UmlInstructionCorpus)
uml_generate_instruction_corpus(uml_arm64_instruction_corpus
    "${CMAKE_CURRENT_SOURCE_DIR}" a64_instruction_corpus.pack
    "${PROJECT_SOURCE_DIR}/tools/arm64-tests/build-unit-corpus.py"
    "${PROJECT_SOURCE_DIR}/tools/arm64-tests/sme-native-corpus.py"
    "${PROJECT_SOURCE_DIR}/tools/arm64-tests/sme_encoding_table.py"
    "${PROJECT_SOURCE_DIR}/tools/arm64-tests/invalid_encoding.py"
    "${PROJECT_SOURCE_DIR}/tests/unit/arm64/a64_instruction_allocations.tsv"
    "${PROJECT_SOURCE_DIR}/tests/unit/arm64/a64_instruction_decode_partitions.tsv"
    "${PROJECT_SOURCE_DIR}/darwin/cpu/arm64/detail/sme_encodings.hpp"
    "${PROJECT_SOURCE_DIR}/darwin/platform/host_capabilities.hpp")
set(UML_ARM64_INSTRUCTION_CORPUS "${uml_arm64_instruction_corpus_PATH}"
    PARENT_SCOPE)
