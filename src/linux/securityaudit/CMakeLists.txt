# Copyright (C) Microsoft Corporation. All rights reserved.

cmake_minimum_required(VERSION 3.25)
project(WslSecurityAuditCollector LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Werror)

get_filename_component(WSL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(MDE_PACKAGE_DIR "" CACHE PATH "Restored architecture-specific ebpf.linux NuGet package")
if(NOT EXISTS "${MDE_PACKAGE_DIR}/include/ebpf/ebpf_plugin_loader.h" OR
   NOT EXISTS "${MDE_PACKAGE_DIR}/lib/libebpf_loader.so")
    message(FATAL_ERROR "MDE_PACKAGE_DIR must contain the matching MDE headers, BPF objects and shared libraries.")
endif()
include(FetchContent)
FetchContent_Declare(nlohmannjson
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmannjson)
find_package(Threads REQUIRED)

add_library(audit_collector_core STATIC Collector.cpp)
target_include_directories(audit_collector_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${WSL_SOURCE_DIR}/src/shared/securityaudit"
    "${MDE_PACKAGE_DIR}/include/ebpf")
target_link_libraries(audit_collector_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads ${CMAKE_DL_LIBS})
add_executable(wsl_audit_collector CollectorMain.cpp)
target_link_libraries(wsl_audit_collector PRIVATE audit_collector_core)
add_executable(audit_syscall_config SyscallConfig.cpp)
target_include_directories(audit_syscall_config PRIVATE "${MDE_PACKAGE_DIR}/include/ebpf")
set(AUDIT_PAYLOAD_DIR "${CMAKE_CURRENT_BINARY_DIR}/payload" CACHE PATH "Output payload directory")

configure_file("${WSL_SOURCE_DIR}/tools/securityaudit/StagePayload.cmake.in" "${CMAKE_CURRENT_BINARY_DIR}/StagePayload.cmake" @ONLY)
add_custom_target(audit_payload
    COMMAND "${CMAKE_COMMAND}" -DCOLLECTOR=$<TARGET_FILE:wsl_audit_collector>
        -DSYSCALL_GENERATOR=$<TARGET_FILE:audit_syscall_config> -P "${CMAKE_CURRENT_BINARY_DIR}/StagePayload.cmake"
    DEPENDS wsl_audit_collector audit_syscall_config
    VERBATIM)
