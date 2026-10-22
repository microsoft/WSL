# Copyright (C) Microsoft Corporation. All rights reserved.

cmake_minimum_required(VERSION 3.25)
project(WslSecurityAuditLaunchTests LANGUAGES CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The listener regressions require native Linux socket/signal semantics.")
endif()
get_filename_component(WSL_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(audit_launch_signal_tests "${WSL_SOURCE_ROOT}/src/linux/securityaudit/tests/LaunchSignalTests.cpp")
target_include_directories(audit_launch_signal_tests PRIVATE "${WSL_SOURCE_ROOT}/src/linux/init")
target_compile_features(audit_launch_signal_tests PRIVATE cxx_std_20)
target_compile_options(audit_launch_signal_tests PRIVATE -Wall -Wextra -Werror)
enable_testing()
add_test(NAME AuditLaunchSignal COMMAND audit_launch_signal_tests)
set_tests_properties(AuditLaunchSignal PROPERTIES TIMEOUT 90)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The production launch decoder requires Clang's -fms-extensions. Set CMAKE_CXX_COMPILER to clang++.")
endif()
find_path(WSL_AUDIT_GSL_INCLUDE gsl/gsl HINTS "${WSL_SOURCE_ROOT}/_deps/x64/gsl-src/include" REQUIRED)
add_executable(audit_launch_request_tests "${WSL_SOURCE_ROOT}/src/linux/securityaudit/tests/LaunchRequestTests.cpp")
target_include_directories(audit_launch_request_tests PRIVATE
    "${WSL_SOURCE_ROOT}/src/linux/init"
    "${WSL_SOURCE_ROOT}/src/linux/inc"
    "${WSL_SOURCE_ROOT}/src/shared/inc"
    "${WSL_SOURCE_ROOT}/src/shared/securityaudit")
target_include_directories(audit_launch_request_tests SYSTEM PRIVATE "${WSL_AUDIT_GSL_INCLUDE}")
target_compile_features(audit_launch_request_tests PRIVATE cxx_std_20)
set_target_properties(audit_launch_request_tests PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(audit_launch_request_tests PRIVATE
    WSL_PACKAGE_VERSION_MAJOR=0 WSL_PACKAGE_VERSION_MINOR=0 WSL_PACKAGE_VERSION_REVISION=0)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    target_compile_definitions(audit_launch_request_tests PRIVATE _AMD64_)
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    target_compile_definitions(audit_launch_request_tests PRIVATE _ARM64_)
else()
    message(FATAL_ERROR "Unsupported launch test architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()
# Keep assertions enabled: a non-null Linux transaction timeout must fail this regression, even in Release.
target_compile_options(audit_launch_request_tests PRIVATE
    -Wall -Werror -fms-extensions -ffunction-sections -fdata-sections -UNDEBUG)
# Unused VM listener startup is discarded; recv is wrapped to emulate Hyper-V's rejection of MSG_PEEK.
target_link_options(audit_launch_request_tests PRIVATE -Wl,--gc-sections -Wl,--wrap=recv)
find_package(Threads REQUIRED)
target_link_libraries(audit_launch_request_tests PRIVATE Threads::Threads)
add_test(NAME AuditLaunchRequest COMMAND audit_launch_request_tests)
set_tests_properties(AuditLaunchRequest PROPERTIES TIMEOUT 90)
