# Copyright (C) Microsoft Corporation. All rights reserved.

set(RUST_TARGET ${WSLOPENVMM_RUST_TARGET})

if ("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
    set(CARGO_PROFILE release)
    set(CARGO_BUILD_ARGUMENTS --release)
else()
    set(CARGO_PROFILE debug)
endif()

set(CARGO_TARGET_DIR ${CMAKE_CURRENT_BINARY_DIR}/cargo-target)
set(CARGO_OUTPUT ${CARGO_TARGET_DIR}/${RUST_TARGET}/${CARGO_PROFILE}/wslopenvmm.dll)
set(CARGO_IMPORT_LIB ${CARGO_TARGET_DIR}/${RUST_TARGET}/${CARGO_PROFILE}/wslopenvmm.dll.lib)
set(WSLOPENVMM_DLL ${BIN}/wslopenvmm.dll)
set(WSLOPENVMM_IMPORT_LIB ${BIN}/wslopenvmm.dll.lib)
set(VM_SERVICE_PROTO "${WSL_DEVICE_HOST_SOURCE_DIR}/proto/VMService.proto")

add_custom_command(
    OUTPUT ${WSLOPENVMM_DLL} ${WSLOPENVMM_IMPORT_LIB}
    COMMAND ${CMAKE_COMMAND} -E env "VM_SERVICE_PROTO=${VM_SERVICE_PROTO}" ${CARGO_EXECUTABLE} build --manifest-path ${CMAKE_CURRENT_LIST_DIR}/Cargo.toml --target ${RUST_TARGET} --target-dir ${CARGO_TARGET_DIR} ${CARGO_BUILD_ARGUMENTS}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CARGO_OUTPUT} ${WSLOPENVMM_DLL}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CARGO_IMPORT_LIB} ${WSLOPENVMM_IMPORT_LIB}
    DEPENDS ${VM_SERVICE_PROTO} Cargo.lock Cargo.toml build.rs src/client.rs src/lib.rs src/af_unix.rs src/rpc.rs src/diagnostics.rs wslopenvmm.h
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    VERBATIM)

add_custom_target(wslopenvmm ALL DEPENDS ${WSLOPENVMM_DLL} ${WSLOPENVMM_IMPORT_LIB})
set_target_properties(wslopenvmm PROPERTIES FOLDER windows SOURCES "Cargo.toml;src/client.rs;src/lib.rs;wslopenvmm.h")

string(CONCAT WSLOPENVMM_TEST_COMMAND
    "\"${CMAKE_COMMAND}\" -E env \"VM_SERVICE_PROTO=${VM_SERVICE_PROTO}\" "
    "\"${CARGO_EXECUTABLE}\" test --manifest-path \"${CMAKE_CURRENT_LIST_DIR}/Cargo.toml\" "
    "--target ${RUST_TARGET} --target-dir \"${CARGO_TARGET_DIR}\" ${CARGO_BUILD_ARGUMENTS} || goto fail")
set(WSLOPENVMM_TEST_COMMAND "${WSLOPENVMM_TEST_COMMAND}" PARENT_SCOPE)