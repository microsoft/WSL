# Copyright (C) Microsoft Corporation. All rights reserved.

set(WSL_SECURITYAUDIT_STAGED_PAYLOAD "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/securityaudit-init")
set(WSL_SECURITYAUDIT_STAGED_PAYLOAD "${WSL_SECURITYAUDIT_STAGED_PAYLOAD}" PARENT_SCOPE)
set(validator "${CMAKE_SOURCE_DIR}/tools/securityaudit/Validate-Payload.ps1")
execute_process(COMMAND powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass
    -File "${validator}" -PayloadDirectory "${SECURITYAUDIT_SOURCE_DIR}"
    -Architecture "${LLVM_ARCH}" -SdkVersion "${LINUXSDK_VERSION}" -PackageVersion "${SECURITYAUDIT_VERSION}" -BuildInputs
    COMMAND_ERROR_IS_FATAL ANY)
include("${CMAKE_SOURCE_DIR}/tools/securityaudit/GenerateBuildInfo.cmake")

add_custom_target(wsl_audit_payload
    COMMAND powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${validator}" -PayloadDirectory "${SECURITYAUDIT_SOURCE_DIR}"
        -Architecture "${LLVM_ARCH}" -SdkVersion "${LINUXSDK_VERSION}" -PackageVersion "${SECURITYAUDIT_VERSION}" -BuildInputs
        -StageDirectory "${WSL_SECURITYAUDIT_STAGED_PAYLOAD}"
    BYPRODUCTS "${WSL_SECURITYAUDIT_STAGED_PAYLOAD}/audit-payload.json"
    VERBATIM)
set_target_properties(wsl_audit_payload PROPERTIES FOLDER linux)

set(SOURCES Collector.cpp CollectorMain.cpp)
set(HEADERS Collector.h "${CMAKE_CURRENT_BINARY_DIR}/AuditBuildInfo.h")
foreach(header event_defs.h ebpf_arch_macros.h ebpf_plugin_loader.h)
    list(APPEND HEADERS "${SECURITYAUDIT_SOURCE_DIR}/include/ebpf/${header}")
endforeach()
set(LINUX_CXXFLAGS ${LINUX_CXXFLAGS}
    -I "${CMAKE_CURRENT_BINARY_DIR}"
    -I "${CMAKE_SOURCE_DIR}/src/shared/securityaudit"
    -I "${SECURITYAUDIT_SOURCE_DIR}/include/ebpf")
add_linux_library(libsecurityaudit "${SOURCES}" "${HEADERS};${COMMON_LINUX_HEADERS}")
add_dependencies(libsecurityaudit wsl_audit_payload)

foreach(library ebpf_loader bpf elf eu z)
    set(input "${SECURITYAUDIT_SOURCE_DIR}/runtimes/linux-musl-${TARGET_PLATFORM}/native/lib${library}.a")
    set(output "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/lib${library}.a")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${input}" "${output}"
        DEPENDS "${input}" wsl_audit_payload VERBATIM)
    add_custom_target(lib${library} DEPENDS "${output}")
    set_target_properties(lib${library} PROPERTIES FOLDER linux)
endforeach()
