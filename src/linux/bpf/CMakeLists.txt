# Copyright (C) Microsoft Corporation. All rights reserved.
#
# CMakeLists.txt for building BPF skeleton headers.
# This is intended to be run on a Linux build machine, NOT as part of the main Windows build.
#
# Required parameters:
#   -DKERNEL_IMAGE_PATH=<path>   Path to the WSL kernel binary (bzImage/Image) with embedded BTF
#   -DBPFTOOL_PATH=<path>        Path to the bpftool binary
#
# Optional parameters:
#   -DCLANG_BPF=<path>           Path to clang (default: clang)
#   -DOUTPUT_DIR=<path>          Output directory for generated headers (default: ${CMAKE_BINARY_DIR}/output)
#
# Usage:
#   cmake -S . -B build -DKERNEL_IMAGE_PATH=/path/to/bzImage -DBPFTOOL_PATH=/usr/sbin/bpftool
#   cmake --build build
#
# Output:
#   ${OUTPUT_DIR}/bind_monitor.skel.h  - BPF skeleton header (embed in NuGet)
#   ${OUTPUT_DIR}/bind_monitor.h       - Shared event struct header (embed in NuGet)

cmake_minimum_required(VERSION 3.20)
project(wsl-bpf LANGUAGES NONE)

# Validate required parameters
if(NOT DEFINED KERNEL_IMAGE_PATH)
    message(FATAL_ERROR "KERNEL_IMAGE_PATH is required. Pass -DKERNEL_IMAGE_PATH=<path to WSL kernel binary>")
endif()

if(NOT EXISTS "${KERNEL_IMAGE_PATH}")
    message(FATAL_ERROR "Kernel image not found: ${KERNEL_IMAGE_PATH}")
endif()

if(NOT DEFINED BPFTOOL_PATH)
    message(FATAL_ERROR "BPFTOOL_PATH is required. Pass -DBPFTOOL_PATH=<path to bpftool>")
endif()

if(NOT EXISTS "${BPFTOOL_PATH}")
    message(FATAL_ERROR "bpftool not found: ${BPFTOOL_PATH}")
endif()

# Optional parameters
if(NOT DEFINED CLANG_BPF)
    set(CLANG_BPF "clang")
endif()

if(NOT DEFINED OUTPUT_DIR)
    set(OUTPUT_DIR "${CMAKE_BINARY_DIR}/output")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(VMLINUX_H "${CMAKE_BINARY_DIR}/vmlinux.h")
set(BPF_OBJ "${CMAKE_BINARY_DIR}/bind_monitor.bpf.o")
set(SKEL_H "${OUTPUT_DIR}/bind_monitor.skel.h")
set(EVENT_H "${OUTPUT_DIR}/bind_monitor.h")

# Detect target architecture for BPF
execute_process(
    COMMAND uname -m
    OUTPUT_VARIABLE HOST_ARCH
    OUTPUT_STRIP_TRAILING_WHITESPACE)

if(HOST_ARCH STREQUAL "x86_64")
    set(BPF_TARGET_ARCH "x86")
elseif(HOST_ARCH STREQUAL "aarch64")
    set(BPF_TARGET_ARCH "arm64")
else()
    message(FATAL_ERROR "Unsupported architecture: ${HOST_ARCH}")
endif()

# Locate libbpf headers (needed for BPF compilation)
find_path(LIBBPF_INCLUDE_DIR "bpf/bpf_helpers.h"
    PATHS /usr/include /usr/local/include
    REQUIRED)

# Step 1: Generate vmlinux.h from kernel image
add_custom_command(
    OUTPUT "${VMLINUX_H}"
    COMMAND bash "${SRC_DIR}/generate-vmlinux-header.sh" "${KERNEL_IMAGE_PATH}" "${VMLINUX_H}"
    DEPENDS "${KERNEL_IMAGE_PATH}" "${SRC_DIR}/generate-vmlinux-header.sh"
    COMMENT "Generating vmlinux.h from kernel image"
    VERBATIM)

# Step 2: Compile BPF program
add_custom_command(
    OUTPUT "${BPF_OBJ}"
    COMMAND "${CLANG_BPF}" -O2 -g -target bpf
            -D__TARGET_ARCH_${BPF_TARGET_ARCH}
            -I "${CMAKE_BINARY_DIR}"
            -I "${LIBBPF_INCLUDE_DIR}"
            -I "${SRC_DIR}"
            -c "${SRC_DIR}/bind_monitor.bpf.c"
            -o "${BPF_OBJ}"
    DEPENDS "${SRC_DIR}/bind_monitor.bpf.c" "${SRC_DIR}/bind_monitor.h" "${VMLINUX_H}"
    COMMENT "Compiling bind_monitor.bpf.c"
    VERBATIM)

# Step 3: Generate skeleton header
add_custom_command(
    OUTPUT "${SKEL_H}"
    COMMAND "${BPFTOOL_PATH}" gen skeleton "${BPF_OBJ}" > "${SKEL_H}"
    DEPENDS "${BPF_OBJ}"
    COMMENT "Generating bind_monitor.skel.h"
    VERBATIM)

# Step 4: Copy shared header to output
add_custom_command(
    OUTPUT "${EVENT_H}"
    COMMAND ${CMAKE_COMMAND} -E copy "${SRC_DIR}/bind_monitor.h" "${EVENT_H}"
    DEPENDS "${SRC_DIR}/bind_monitor.h"
    COMMENT "Copying bind_monitor.h to output"
    VERBATIM)

# Main target
add_custom_target(bpf_headers ALL DEPENDS "${SKEL_H}" "${EVENT_H}")

message(STATUS "BPF build configuration:")
message(STATUS "  Kernel image:  ${KERNEL_IMAGE_PATH}")
message(STATUS "  bpftool:       ${BPFTOOL_PATH}")
message(STATUS "  clang:         ${CLANG_BPF}")
message(STATUS "  Target arch:   ${BPF_TARGET_ARCH}")
message(STATUS "  Output dir:    ${OUTPUT_DIR}")
