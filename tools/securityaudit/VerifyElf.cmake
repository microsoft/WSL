# Copyright (C) Microsoft Corporation. All rights reserved.

find_program(readelf readelf REQUIRED)
execute_process(COMMAND "${readelf}" -l -d "${COLLECTOR}" OUTPUT_VARIABLE elf
    ERROR_VARIABLE error COMMAND_ERROR_IS_FATAL ANY)
if(elf MATCHES "INTERP|\\(NEEDED\\)")
    message(FATAL_ERROR "Collector is not fully static: ${elf}")
endif()
file(READ "${COLLECTOR}" machine OFFSET 18 LIMIT 2 HEX)
if((ARCHITECTURE STREQUAL "x86_64" AND NOT machine STREQUAL "3e00") OR
   (ARCHITECTURE STREQUAL "aarch64" AND NOT machine STREQUAL "b700") OR
   NOT ARCHITECTURE MATCHES "^(x86_64|aarch64)$")
    message(FATAL_ERROR "Collector architecture mismatch: ${machine}")
endif()
message(STATUS "Verified ${ARCHITECTURE} collector: no PT_INTERP or DT_NEEDED")
