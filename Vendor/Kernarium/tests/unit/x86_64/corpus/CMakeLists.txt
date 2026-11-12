# SPDX-License-Identifier: GPL-2.0-or-later

include(UmlInstructionCorpus)
uml_generate_instruction_corpus(uml_x86_64_instruction_corpus
    "${CMAKE_CURRENT_SOURCE_DIR}" x86_64_v3.pack
    "${PROJECT_SOURCE_DIR}/tools/x86-tests/build-unit-corpus.py"
    "${PROJECT_SOURCE_DIR}/tools/fex-tests/fex_config.py"
    "${PROJECT_SOURCE_DIR}/tools/fex-tests/pinned_source.py")
set(UML_X86_64_INSTRUCTION_CORPUS "${uml_x86_64_instruction_corpus_PATH}"
    PARENT_SCOPE)
