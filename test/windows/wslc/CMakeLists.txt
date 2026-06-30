# WSLC CLI Unit Tests

file(GLOB_RECURSE WSLC_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

file(GLOB_RECURSE WSLC_TEST_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h
)

# Add the sources to the parent wsltests target.
# This ensures they're compiled into the main test binary.
target_sources(wsltests PRIVATE ${WSLC_TEST_SOURCES} ${WSLC_TEST_HEADERS})

# Ensure the file uses the precompiled header from the parent target.
set_source_files_properties(${WSLC_TEST_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "/Yuprecomp.h"
)

# Opt-in unity build for the e2e tests (~2x faster clean recompile; CI enables it).
option(WSL_E2E_UNITY_BUILD "Unity-batch the wslc e2e test sources" OFF)
set(WSL_E2E_UNITY_BATCH_SIZE 8 CACHE STRING "Unity batch size for wslc e2e tests")

if(WSL_E2E_UNITY_BUILD)
    # GROUP mode only batches sources tagged with UNITY_GROUP, so tag e2e sources and leave every other wsltests source untouched.
    set_target_properties(wsltests PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)

    file(GLOB WSLC_E2E_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/e2e/*.cpp)
    set(_e2eIndex 0)
    foreach(_e2eSource IN LISTS WSLC_E2E_SOURCES)
        math(EXPR _e2eGroup "${_e2eIndex} / ${WSL_E2E_UNITY_BATCH_SIZE}")
        set_source_files_properties(${_e2eSource}
            TARGET_DIRECTORY wsltests
            PROPERTIES UNITY_GROUP "wslc_e2e_${_e2eGroup}"
        )
        math(EXPR _e2eIndex "${_e2eIndex} + 1")
    endforeach()
endif()

# Add include directories needed for WSLC tests.
target_include_directories(wsltests PRIVATE
    ${CMAKE_SOURCE_DIR}/test
    ${CMAKE_SOURCE_DIR}/test/windows
    ${CMAKE_SOURCE_DIR}/test/windows/wslc
    ${CMAKE_SOURCE_DIR}/src/windows/wslc/core
    ${CMAKE_SOURCE_DIR}/src/windows/wslc/commands
    ${CMAKE_SOURCE_DIR}/src/windows/wslc/arguments
    ${CMAKE_SOURCE_DIR}/src/windows/wslc/services
    ${CMAKE_SOURCE_DIR}/src/windows/wslc/tasks
)
