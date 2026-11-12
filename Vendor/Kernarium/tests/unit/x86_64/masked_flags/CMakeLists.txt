# SPDX-License-Identifier: GPL-2.0-or-later

include(UmlInstructionCorpus)
uml_generate_instruction_corpus(uml_x86_64_masked_flags
    "${CMAKE_CURRENT_SOURCE_DIR}" xeon-flags.bin)
set(UML_X86_64_MASKED_FLAGS "${uml_x86_64_masked_flags_PATH}" PARENT_SCOPE)
