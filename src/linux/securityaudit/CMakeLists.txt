# Copyright (C) Microsoft Corporation. All rights reserved.

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SECURITYAUDIT_SOURCE_DIR}/metadata/link.json")
file(READ "${SECURITYAUDIT_SOURCE_DIR}/metadata/link.json" auditLink)
string(JSON auditSdk GET "${auditLink}" sdk)
string(JSON auditTarget GET "${auditLink}" target)
if(NOT auditSdk STREQUAL "Microsoft.WSL.LinuxSdk/${LINUXSDK_VERSION}")
    message(FATAL_ERROR "Audit SDK ${auditSdk} does not match Microsoft.WSL.LinuxSdk/${LINUXSDK_VERSION}.")
endif()
if(NOT auditTarget STREQUAL "${LLVM_ARCH}-unknown-linux-musl")
    message(FATAL_ERROR "Audit target ${auditTarget} does not match ${LLVM_ARCH}-unknown-linux-musl.")
endif()

set(SOURCES Collector.cpp CollectorMain.cpp)
set(HEADERS Collector.h)
foreach(header event_defs.h ebpf_arch_macros.h ebpf_plugin_loader.h)
    list(APPEND HEADERS "${SECURITYAUDIT_SOURCE_DIR}/include/ebpf/${header}")
endforeach()
set(LINUX_CXXFLAGS ${LINUX_CXXFLAGS}
    -I "${CMAKE_SOURCE_DIR}/src/shared/securityaudit"
    -I "${SECURITYAUDIT_SOURCE_DIR}/include/ebpf")
add_linux_library(libsecurityaudit "${SOURCES}" "${HEADERS};${COMMON_LINUX_HEADERS}")

foreach(library ebpf_loader bpf elf eu z)
    set(input "${SECURITYAUDIT_SOURCE_DIR}/runtimes/linux-musl-${TARGET_PLATFORM}/native/lib${library}.a")
    set(output "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/lib${library}.a")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${input}" "${output}"
        DEPENDS "${input}" VERBATIM)
    add_custom_target(lib${library} DEPENDS "${output}")
    set_target_properties(lib${library} PROPERTIES FOLDER linux)
endforeach()
