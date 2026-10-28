# Copyright (C) Microsoft Corporation. All rights reserved.

if(TARGET_PLATFORM STREQUAL "x64")
    set(AUDIT_DEB_ARCH amd64)
    set(AUDIT_TRIPLET x86_64-linux-gnu)
    set(AUDIT_LOADER ld-linux-x86-64.so.2)
    set(AUDIT_SYSCALLS syscalls.conf)
    find_nuget_package(ebpf.linux-x86_64 MDE /)
else()
    set(AUDIT_DEB_ARCH arm64)
    set(AUDIT_TRIPLET aarch64-linux-gnu)
    set(AUDIT_LOADER ld-linux-aarch64.so.1)
    set(AUDIT_SYSCALLS syscalls_aarch64.conf)
    find_nuget_package(ebpf.linux-arm64 MDE /)
endif()
include("${CMAKE_SOURCE_DIR}/cmake/SecurityAuditSysroot.cmake")

set(AUDIT_PAYLOAD_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/securityaudit")
set(AUDIT_TOOLS_DIR "${CMAKE_SOURCE_DIR}/tools/securityaudit")
set(collector "${AUDIT_PAYLOAD_DIR}/wsl_audit_collector")
set(collector_symbols "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/wsl_audit_collector.debug")
set(compiler_flags
    "--target=${AUDIT_TRIPLET}"
    "--sysroot=${AUDIT_SYSROOT}"
    "--gcc-install-dir=${AUDIT_SYSROOT}/usr/lib/gcc/${AUDIT_TRIPLET}/13"
    -std=c++20 -pthread -fPIE -Wall -Wextra -Werror
    -I "${CMAKE_SOURCE_DIR}/src/shared/securityaudit"
    -I "${MDE_SOURCE_DIR}/include/ebpf"
    -I "${NLOHMAN_JSON_SOURCE_DIR}/include")
file(GLOB audit_headers CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/shared/securityaudit/*.h")
list(APPEND audit_headers "${CMAKE_CURRENT_SOURCE_DIR}/Collector.h"
    "${MDE_SOURCE_DIR}/include/ebpf/ebpf_plugin_loader.h"
    "${MDE_SOURCE_DIR}/include/ebpf/event_defs.h"
    "${MDE_SOURCE_DIR}/include/ebpf/ebpf_arch_macros.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/NOTICE.txt")

configure_file("${AUDIT_TOOLS_DIR}/StagePayload.cmake.in" "${CMAKE_CURRENT_BINARY_DIR}/StagePayload.cmake" @ONLY)
execute_process(COMMAND "${CMAKE_COMMAND}" -DPREPARE=ON -P "${CMAKE_CURRENT_BINARY_DIR}/StagePayload.cmake"
    COMMAND_ERROR_IS_FATAL ANY)

add_custom_command(OUTPUT "${collector}" "${collector_symbols}"
    COMMAND "${LINUX_CXX}" ${compiler_flags} ${LINUX_BUILD_TYPE_FLAGS}
        "${CMAKE_CURRENT_SOURCE_DIR}/Collector.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/CollectorMain.cpp"
        -fuse-ld=lld -pie -Wl,-z,relro,-z,now -Wl,--build-id=sha1 -ldl -o "${collector_symbols}"
    COMMAND "${LLVM_INSTALL_DIR}/llvm-strip.exe" -o "${collector}" "${collector_symbols}"
    COMMAND "${LLVM_INSTALL_DIR}/llvm-objcopy.exe" --add-gnu-debuglink "${collector_symbols}" "${collector}"
    DEPENDS Collector.cpp CollectorMain.cpp ${audit_headers} "${sysroot_manifest}"
    VERBATIM)
add_custom_command(OUTPUT "${AUDIT_PAYLOAD_DIR}/${AUDIT_SYSCALLS}"
    COMMAND "${LINUX_CXX}" ${compiler_flags} -E -P -x c++ "${CMAKE_CURRENT_SOURCE_DIR}/SyscallConfig.in"
        -o "${CMAKE_CURRENT_BINARY_DIR}/syscalls.preprocessed"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/syscalls.preprocessed"
        "-DOUTPUT=${AUDIT_PAYLOAD_DIR}/${AUDIT_SYSCALLS}" -P "${AUDIT_TOOLS_DIR}/GenerateSyscalls.cmake"
    DEPENDS SyscallConfig.in "${AUDIT_TOOLS_DIR}/GenerateSyscalls.cmake" ${audit_headers}
    VERBATIM)

file(GLOB_RECURSE payload_files "${AUDIT_PAYLOAD_DIR}/*")
list(APPEND payload_files "${collector}" "${AUDIT_PAYLOAD_DIR}/${AUDIT_SYSCALLS}" "${AUDIT_PAYLOAD_DIR}/payload.json")
list(REMOVE_DUPLICATES payload_files)
set(manifest_inputs ${payload_files})
list(REMOVE_ITEM manifest_inputs "${AUDIT_PAYLOAD_DIR}/payload.json")
add_custom_command(OUTPUT "${AUDIT_PAYLOAD_DIR}/payload.json"
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/StagePayload.cmake"
    DEPENDS ${manifest_inputs} ${audit_headers} Collector.cpp CollectorMain.cpp SyscallConfig.in
        CMakeLists.txt "${sysroot_manifest}" "${CMAKE_SOURCE_DIR}/packages.config"
        "${CMAKE_SOURCE_DIR}/cmake/SecurityAudit.cmake"
        "${CMAKE_SOURCE_DIR}/cmake/SecurityAuditSysroot.cmake"
        "${AUDIT_TOOLS_DIR}/Validate-Payload.ps1" "${AUDIT_TOOLS_DIR}/GenerateSyscalls.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/StagePayload.cmake"
    VERBATIM)
add_custom_target(securityauditpayload DEPENDS "${AUDIT_PAYLOAD_DIR}/payload.json"
    COMMAND powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${AUDIT_TOOLS_DIR}/Validate-Payload.ps1"
        -PayloadDirectory "${AUDIT_PAYLOAD_DIR}" -Architecture "${TARGET_PLATFORM}" -SourceDirectory "${CMAKE_SOURCE_DIR}"
    SOURCES Collector.cpp CollectorMain.cpp Collector.h SyscallConfig.in
    VERBATIM)
set_target_properties(securityauditpayload PROPERTIES FOLDER linux)
set(SECURITYAUDIT_PAYLOAD_DIR "${AUDIT_PAYLOAD_DIR}" PARENT_SCOPE)
set(SECURITYAUDIT_PAYLOAD_FILES "${payload_files}" PARENT_SCOPE)
