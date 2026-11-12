# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED OUTPUT OR OUTPUT STREQUAL "")
    message(FATAL_ERROR "OUTPUT is required")
endif()
if(NOT EXISTS "${OUTPUT}")
    message(FATAL_ERROR "Expected generated file is missing: ${OUTPUT}")
endif()
file(SIZE "${OUTPUT}" _generated_size)
if(_generated_size EQUAL 0)
    message(FATAL_ERROR "Expected generated file is empty: ${OUTPUT}")
endif()
