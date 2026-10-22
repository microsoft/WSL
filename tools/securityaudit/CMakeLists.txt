# Copyright (C) Microsoft Corporation. All rights reserved.

cmake_minimum_required(VERSION 3.25)
project(WslSecurityAuditPayload LANGUAGES CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Build this independent static payload on Linux, then pass it to the Windows WSL build.")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|aarch64)$")
    message(FATAL_ERROR "Unsupported audit payload architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()
get_filename_component(WSL_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(WSL_AUDIT_EBPF_SOURCE_ROOT "" CACHE PATH "Current WD.Client.Linux.eBPF source")
set(WSL_AUDIT_EBPF_STATIC_PACKAGE "" CACHE PATH "Output of the eBPF build.sh -S")
set(WSL_AUDIT_JSON_INCLUDE "" CACHE PATH "Directory containing nlohmann/json.hpp (WSL's pinned 3.12.0)")
set(WSL_AUDIT_PAYLOAD_OUTPUT "" CACHE PATH "Destination for the validated boot payload")
foreach(required
    "${WSL_AUDIT_EBPF_SOURCE_ROOT}/3rdpartycode/event_defs.h"
    "${WSL_AUDIT_EBPF_STATIC_PACKAGE}/libebpf_loader.a"
    "${WSL_AUDIT_EBPF_STATIC_PACKAGE}/libbpf.a"
    "${WSL_AUDIT_JSON_INCLUDE}/nlohmann/json.hpp")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing static audit build dependency: ${required}. See tools/securityaudit/README.md.")
    endif()
endforeach()
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ELF REQUIRED libelf)
# libelf's compression dependencies differ by distro; require each transitive archive, never a .so fallback.
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(audit_archives "")
foreach(library IN LISTS ELF_STATIC_LIBRARIES)
    find_library(archive_${library} NAMES "${library}" HINTS ${ELF_STATIC_LIBRARY_DIRS} REQUIRED)
    list(APPEND audit_archives "${archive_${library}}")
endforeach()
find_library(audit_zlib NAMES z REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/GenerateBuildInfo.cmake")
add_library(wsl_audit_collector_core STATIC "${WSL_SOURCE_ROOT}/src/linux/securityaudit/Collector.cpp")
target_compile_features(wsl_audit_collector_core PUBLIC cxx_std_20)
target_compile_options(wsl_audit_collector_core PRIVATE -Wall -Wextra -Werror -fstack-protector-strong)
target_include_directories(wsl_audit_collector_core PUBLIC
    "${WSL_SOURCE_ROOT}/src/linux/securityaudit"
    "${WSL_SOURCE_ROOT}/src/shared/securityaudit"
    "${WSL_AUDIT_EBPF_SOURCE_ROOT}/3rdpartycode"
    "${WSL_AUDIT_EBPF_SOURCE_ROOT}/ebpf_loader/src"
    "${WSL_AUDIT_JSON_INCLUDE}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(wsl_audit_collector_core PUBLIC
    "${WSL_AUDIT_EBPF_STATIC_PACKAGE}/libebpf_loader.a"
    "${WSL_AUDIT_EBPF_STATIC_PACKAGE}/libbpf.a"
    ${audit_archives} "${audit_zlib}" Threads::Threads)
target_link_options(wsl_audit_collector_core INTERFACE -static -Wl,--no-dynamic-linker,-z,relro,-z,now)
add_executable(wsl_audit_collector "${WSL_SOURCE_ROOT}/src/linux/securityaudit/CollectorMain.cpp")
target_link_libraries(wsl_audit_collector PRIVATE wsl_audit_collector_core)
include(CTest)
if(BUILD_TESTING)
    add_executable(audit_collector_tests "${WSL_SOURCE_ROOT}/src/linux/securityaudit/tests/CollectorTests.cpp")
    target_link_libraries(audit_collector_tests PRIVATE wsl_audit_collector_core)
    add_test(NAME AuditCollector COMMAND audit_collector_tests)
    foreach(test Protocol Observation)
        string(TOLOWER "${test}" lower)
        add_executable(audit_${lower}_tests "${WSL_SOURCE_ROOT}/src/shared/securityaudit/tests/${test}Tests.cpp")
        target_link_libraries(audit_${lower}_tests PRIVATE wsl_audit_collector_core)
        add_test(NAME Audit${test} COMMAND audit_${lower}_tests)
    endforeach()
    add_executable(audit_live_collector_test "${WSL_SOURCE_ROOT}/src/linux/securityaudit/tests/LiveCollectorTest.cpp")
    target_link_libraries(audit_live_collector_test PRIVATE wsl_audit_collector_core)
    option(WSL_AUDIT_ENABLE_LIVE_TESTS "Run privileged BPF loading and real process-event tests" OFF)
    if(WSL_AUDIT_ENABLE_LIVE_TESTS)
        add_test(NAME AuditLiveCollector COMMAND audit_live_collector_test
            "$<TARGET_FILE:wsl_audit_collector>" "${WSL_AUDIT_PAYLOAD_OUTPUT}/wsl-security-audit")
        set_tests_properties(AuditLiveCollector PROPERTIES TIMEOUT 300 LABELS "privileged;integration")
        add_test(NAME AuditMinimalRootNetwork COMMAND bash "${CMAKE_CURRENT_LIST_DIR}/test-minimal-root.sh"
            "$<TARGET_FILE:audit_live_collector_test>" "${WSL_AUDIT_PAYLOAD_OUTPUT}"
            "${CMAKE_CURRENT_BINARY_DIR}/minimal-audit-root")
        set_tests_properties(AuditMinimalRootNetwork PROPERTIES TIMEOUT 300 LABELS "privileged;integration")
    endif()
    add_test(NAME AuditStaticElf COMMAND "${CMAKE_COMMAND}"
        "-DCOLLECTOR=$<TARGET_FILE:wsl_audit_collector>"
        "-DARCHITECTURE=${CMAKE_SYSTEM_PROCESSOR}" -P "${CMAKE_CURRENT_LIST_DIR}/VerifyElf.cmake")
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/StagePayload.cmake.in" "${CMAKE_CURRENT_BINARY_DIR}/StagePayload.cmake" @ONLY)
add_custom_target(audit_payload
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/StagePayload.cmake"
    DEPENDS wsl_audit_collector VERBATIM)
