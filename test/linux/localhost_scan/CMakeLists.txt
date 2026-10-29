set(SOURCES
    LocalhostScanTests.cpp
    ../../../src/linux/init/util.cpp)

set(LINUX_CXXFLAGS ${LINUX_CXXFLAGS}
    -I "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/linux/init"
    -I "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/linux/netlinkutil"
    -ffunction-sections -fdata-sections)
set(LINUX_LDFLAGS ${LINUX_LDFLAGS} -Wl,--gc-sections -Wl,--wrap=socket,--wrap=bind,--wrap=nanosleep)

add_linux_executable(localhost_scan_tests "${SOURCES}" "${COMMON_LINUX_HEADERS};${CMAKE_CURRENT_SOURCE_DIR}/../../../src/linux/init/localhost.cpp" "netlinkutil")
add_dependencies(localhost_scan_tests localization)
