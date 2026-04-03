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
    ${CMAKE_SOURCE_DIR}/src/windows/wslc/settings
)
