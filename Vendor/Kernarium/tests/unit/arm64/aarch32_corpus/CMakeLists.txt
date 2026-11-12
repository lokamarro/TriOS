# SPDX-License-Identifier: GPL-2.0-or-later

include(UmlInstructionCorpus)
uml_generate_instruction_corpus(uml_arm64_aarch32_corpus
    "${CMAKE_CURRENT_SOURCE_DIR}" a32_instruction_corpus.pack
    "${PROJECT_SOURCE_DIR}/tools/instruction-corpus/aarch32.py")
set(UML_ARM64_AARCH32_CORPUS "${uml_arm64_aarch32_corpus_PATH}" PARENT_SCOPE)
