# Copyright (C) Microsoft Corporation. All rights reserved.

# Standalone build: this is a sample SDK consumer, not part of the product, so it is not wired
# into the root CMakeLists. It builds against the headers in the repository and the import library
# produced by a normal WSL build.

cmake_minimum_required(VERSION 3.25)
project(wslccrashdemo CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(WSLC_SDK_INCLUDE_DIR "${REPO_ROOT}/src/windows/WslcSDK" CACHE PATH "Directory containing wslcsdk.h")
set(WSLC_SDK_BINARY_DIR "${REPO_ROOT}/bin/x64/Debug" CACHE PATH "Directory containing wslcsdk.lib and wslcsdk.dll")

# The name the Watson report is filed under: WSLAppCrash P1 is the file name of the process that
# created the container.
set(DEMO_APP_NAME "wingetexe" CACHE STRING "Output name of the demo application")

add_executable(wslccrashdemo main.cpp)
set_target_properties(wslccrashdemo PROPERTIES OUTPUT_NAME "${DEMO_APP_NAME}")
target_include_directories(wslccrashdemo PRIVATE "${WSLC_SDK_INCLUDE_DIR}")
target_link_libraries(wslccrashdemo PRIVATE "${WSLC_SDK_BINARY_DIR}/wslcsdk.lib")

# The workload that runs inside the container is a Linux program, so it is cross compiled with the
# same clang and musl sysroot the repository uses for its own Linux binaries. Linking it statically
# means it does not care which libc the container image ships, so any image can run it.
file(GLOB LINUX_SYSROOT_CANDIDATES "${REPO_ROOT}/packages/Microsoft.WSL.LinuxSdk.*/x86_64")
list(SORT LINUX_SYSROOT_CANDIDATES)
list(REVERSE LINUX_SYSROOT_CANDIDATES)
list(GET LINUX_SYSROOT_CANDIDATES 0 LINUX_SYSROOT_DEFAULT)

file(GLOB CLANG_CANDIDATES "$ENV{ProgramFiles}/Microsoft Visual Studio/*/*/VC/Tools/Llvm/x64/bin/clang.exe")
list(SORT CLANG_CANDIDATES)
list(REVERSE CLANG_CANDIDATES)
list(GET CLANG_CANDIDATES 0 CLANG_DEFAULT)

set(WSLC_LINUX_SYSROOT "${LINUX_SYSROOT_DEFAULT}" CACHE PATH "musl sysroot used to build the container workload")
set(WSLC_LINUX_CC "${CLANG_DEFAULT}" CACHE FILEPATH "clang used to build the container workload")

if (NOT EXISTS "${WSLC_LINUX_SYSROOT}")
    message(FATAL_ERROR "Linux sysroot not found. Build the repository once so the LinuxSdk package is restored, or set WSLC_LINUX_SYSROOT.")
endif()

if (NOT EXISTS "${WSLC_LINUX_CC}")
    message(FATAL_ERROR "clang.exe not found. Install the C++ Clang Compiler for Windows, or set WSLC_LINUX_CC.")
endif()

# The startup files and libraries are named explicitly because the driver otherwise looks for a gcc
# installation next to the sysroot and fails on the crtbegin/crtend files that musl does not ship.
set(WORKLOAD_BINARY "${CMAKE_CURRENT_BINARY_DIR}/payload/crash")
add_custom_command(
    OUTPUT "${WORKLOAD_BINARY}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/payload"
    COMMAND "${WSLC_LINUX_CC}"
            -target x86_64-unknown-linux-musl
            -isysroot "${WSLC_LINUX_SYSROOT}"
            -isystem "${WSLC_LINUX_SYSROOT}/include"
            -fuse-ld=lld.exe
            -static -g -O2 -Wall
            -nostdlib -nostartfiles
            "${WSLC_LINUX_SYSROOT}/lib/crt1.o"
            "${WSLC_LINUX_SYSROOT}/lib/crti.o"
            "${CMAKE_CURRENT_SOURCE_DIR}/crash.c"
            "-L${WSLC_LINUX_SYSROOT}/lib"
            "-L${WSLC_LINUX_SYSROOT}/lib/linux"
            "-l:libc.a"
            "-l:libclang_rt.builtins-x86_64.a"
            "${WSLC_LINUX_SYSROOT}/lib/crtn.o"
            -o "${WORKLOAD_BINARY}"
    MAIN_DEPENDENCY "${CMAKE_CURRENT_SOURCE_DIR}/crash.c"
    COMMENT "Cross compiling the Linux crash workload"
    VERBATIM)

add_custom_target(crashworkload DEPENDS "${WORKLOAD_BINARY}")
add_dependencies(wslccrashdemo crashworkload)

# The application mounts this directory into the container, so the workload has to sit next to the
# executable in a known place.
add_custom_command(TARGET wslccrashdemo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WORKLOAD_BINARY}"
        "$<TARGET_FILE_DIR:wslccrashdemo>/payload/crash")

# wslcsdk.dll is not installed with WSL; an application that uses the SDK ships it alongside its
# own binary, so copy it next to the executable.
add_custom_command(TARGET wslccrashdemo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WSLC_SDK_BINARY_DIR}/wslcsdk.dll"
        "$<TARGET_FILE_DIR:wslccrashdemo>")
