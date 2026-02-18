# WSLC CLI Unit Tests

set(WSLC_TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/WSLCCLIParserUnitTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/WSLCCLIArgumentUnitTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/WSLCCLICommandUnitTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/WSLCCLIExecutionUnitTests.cpp
)

set(WSLC_TEST_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/WSLCCLITestHelpers.h
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandLineTestCases.h 
    ${CMAKE_CURRENT_SOURCE_DIR}/ParserTestCases.h
)

# Add the sources to the parent wsltests target.
# This ensures they're compiled into the main test binary.
target_sources(wsltests PRIVATE ${WSLC_TEST_SOURCES})

# Ensure the file uses the precompiled header from the parent target.
set_source_files_properties(${WSLC_TEST_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "/Yuprecomp.h"
)

# Add include directories needed for WSLC tests.
target_include_directories(wsltests PRIVATE
    ${CMAKE_SOURCE_DIR}/test
    ${CMAKE_SOURCE_DIR}/test/windows
    ${CMAKE_SOURCE_DIR}/src/windows/wslc/core
    ${CMAKE_SOURCE_DIR}/src/windows/wslc/commands
    ${CMAKE_SOURCE_DIR}/src/windows/wslc/arguments
    ${CMAKE_SOURCE_DIR}/src/windows/wslc/tasks
)
