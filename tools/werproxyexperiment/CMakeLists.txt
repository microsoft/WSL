# Copyright (C) Microsoft Corporation. All rights reserved.
#
# Standalone project. It is deliberately not part of the WSL build: it has no dependency on
# anything in this repository, so it configures and builds on its own in seconds.
#
#   cmake -S tools\werproxyexperiment -B tools\werproxyexperiment\build
#   cmake --build tools\werproxyexperiment\build --config Debug

cmake_minimum_required(VERSION 3.25)

project(werproxyexperiment CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The name the target process runs under decides which team owns the resulting Watson bucket, so it
# is the knob this experiment is built around.
set(SPINTARGET_NAME "winget" CACHE STRING "Output name of the process being reported against")
set(SPINTARGET_PRODUCT "Windows Package Manager" CACHE STRING "ProductName in the target's version resource")

add_executable(spintarget spintarget.cpp spintarget.rc)
set_target_properties(spintarget PROPERTIES OUTPUT_NAME "${SPINTARGET_NAME}")
target_compile_definitions(
    spintarget
    PRIVATE UNICODE
            _UNICODE
            NOMINMAX
            SPINTARGET_FILE_NAME="${SPINTARGET_NAME}.exe"
            SPINTARGET_PRODUCT_NAME="${SPINTARGET_PRODUCT}")

add_executable(proxyreport proxyreport.cpp)
target_compile_definitions(proxyreport PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(proxyreport PRIVATE wer.lib psapi.lib)
