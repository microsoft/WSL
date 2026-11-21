file(GLOB_RECURSE PRODUCTION_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/linux/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/linux/*.hxx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/shared/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/shared/*.hxx")

set(LINUX_CXXFLAGS ${LINUX_CXXFLAGS}
    -I "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/linux/init"
    -I "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/linux/netlinkutil")

add_linux_executable(netlink_dump_tests "NetlinkDumpTests.cpp" "${PRODUCTION_HEADERS}" "netlinkutil")
add_dependencies(wsltests netlink_dump_tests)
