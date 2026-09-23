# WSLC Tests
#
# Shared helpers live in helpers/. Unit tests live in unit/, grouped by the layer under
# test: arguments/, commands/, models/, cli/ and common/. End-to-end tests live in
# e2e/, grouped by how they drive the product: e2e/service/ calls the COM API directly,
# e2e/cli/ runs wslc.exe and e2e/sdk/ exercises the WslcSDK and its WinRT projection.

file(GLOB_RECURSE WSLC_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

file(GLOB_RECURSE WSLC_TEST_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h
)

# Add the sources to the parent wsltests target.
# This ensures they're compiled into the main test binary.
target_sources(wsltests PRIVATE ${WSLC_TEST_SOURCES} ${WSLC_TEST_HEADERS})

# N.B. These sources are compiled into wsltests, which is defined in the parent
# directory. Source file properties are scoped to the directory that sets them,
# so they would be ignored here. The precompiled header comes from the target
# instead, via target_precompile_headers(wsltests REUSE_FROM common).

# Add include directories needed for WSLC tests.
target_include_directories(wsltests PRIVATE
    ${CMAKE_SOURCE_DIR}/test
    ${CMAKE_SOURCE_DIR}/test/windows
    ${CMAKE_SOURCE_DIR}/test/windows/common
    ${CMAKE_SOURCE_DIR}/test/windows/wslc
    ${CMAKE_SOURCE_DIR}/test/windows/wslc/helpers
    ${CMAKE_SOURCE_DIR}/test/windows/wslc/unit
    ${CMAKE_SOURCE_DIR}/test/windows/wslc/e2e
    ${CMAKE_SOURCE_DIR}/src/windows/wslc/core
    ${CMAKE_SOURCE_DIR}/src/windows/wslc/commands
    ${CMAKE_SOURCE_DIR}/src/windows/wslc/arguments
    ${CMAKE_SOURCE_DIR}/src/windows/wslc/services
    ${CMAKE_SOURCE_DIR}/src/windows/wslc/tasks
)
