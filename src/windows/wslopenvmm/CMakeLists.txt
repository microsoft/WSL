# Copyright (C) Microsoft Corporation. All rights reserved.

find_program(CARGO_EXECUTABLE cargo REQUIRED)

if ("${TARGET_PLATFORM}" STREQUAL "x64")
    set(RUST_TARGET x86_64-pc-windows-msvc)
elseif ("${TARGET_PLATFORM}" STREQUAL "arm64")
    set(RUST_TARGET aarch64-pc-windows-msvc)
else()
    message(FATAL_ERROR "Unsupported Rust target platform: '${TARGET_PLATFORM}'")
endif()

if ("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
    set(CARGO_PROFILE release)
    set(CARGO_BUILD_ARGUMENTS --release)
else()
    set(CARGO_PROFILE debug)
endif()

set(CARGO_TARGET_DIR ${CMAKE_CURRENT_BINARY_DIR}/cargo-target)
set(CARGO_OUTPUT ${CARGO_TARGET_DIR}/${RUST_TARGET}/${CARGO_PROFILE}/wslopenvmm.dll)
set(WSLOPENVMM_DLL ${BIN}/wslopenvmm.dll)
set(WSLOPENVMM_IMPLIB ${CARGO_TARGET_DIR}/${RUST_TARGET}/${CARGO_PROFILE}/wslopenvmm.dll.lib)
set(VM_SERVICE_PROTO "${WSL_DEVICE_HOST_SOURCE_DIR}/proto/VMService.proto" CACHE FILEPATH "OpenVMM RPC protocol schema")

add_custom_command(
    OUTPUT ${WSLOPENVMM_DLL}
    BYPRODUCTS ${WSLOPENVMM_IMPLIB}
    COMMAND ${CMAKE_COMMAND} -E env "VM_SERVICE_PROTO=${VM_SERVICE_PROTO}" ${CARGO_EXECUTABLE} build --manifest-path ${CMAKE_CURRENT_LIST_DIR}/Cargo.toml --target ${RUST_TARGET} --target-dir ${CARGO_TARGET_DIR} ${CARGO_BUILD_ARGUMENTS}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CARGO_OUTPUT} ${WSLOPENVMM_DLL}
    DEPENDS ${VM_SERVICE_PROTO} Cargo.lock Cargo.toml build.rs src/client.rs src/lib.rs src/named_pipe.rs wslopenvmm.h
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    VERBATIM)

add_custom_target(wslopenvmm ALL DEPENDS ${WSLOPENVMM_DLL})
set_target_properties(wslopenvmm PROPERTIES FOLDER windows SOURCES "Cargo.toml;src/client.rs;src/lib.rs;wslopenvmm.h")

add_library(wslopenvmm_client SHARED IMPORTED GLOBAL)
set_target_properties(wslopenvmm_client PROPERTIES
    IMPORTED_LOCATION "${WSLOPENVMM_DLL}"
    IMPORTED_IMPLIB "${WSLOPENVMM_IMPLIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}")
add_dependencies(wslopenvmm_client wslopenvmm)