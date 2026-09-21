# WSL Tests
#
# Unit tests live in unit/ and end-to-end tests live in e2e/.

file(GLOB_RECURSE WSL_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

file(GLOB_RECURSE WSL_TEST_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h
)

# Add the sources to the parent wsltests target.
# This ensures they're compiled into the main test binary.
target_sources(wsltests PRIVATE ${WSL_TEST_SOURCES} ${WSL_TEST_HEADERS})

# N.B. These sources are compiled into wsltests, which is defined in the parent
# directory. Source file properties are scoped to the directory that sets them,
# so per-source include directories are set there instead. The precompiled header
# comes from the target, via target_precompile_headers(wsltests REUSE_FROM common).

# Add include directories needed for WSL tests.
target_include_directories(wsltests PRIVATE
    ${CMAKE_SOURCE_DIR}/test
    ${CMAKE_SOURCE_DIR}/test/windows
    ${CMAKE_SOURCE_DIR}/test/windows/common
    ${CMAKE_SOURCE_DIR}/test/windows/wsl/unit
    ${CMAKE_SOURCE_DIR}/test/windows/wsl/e2e
)
