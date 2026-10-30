set(SOURCES
    LocalhostStartTests.cpp
    ../../../src/linux/init/util.cpp)

file(GLOB_RECURSE PRODUCTION_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/linux/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/linux/*.hxx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/shared/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/shared/*.hxx")

set(LINUX_CXXFLAGS ${LINUX_CXXFLAGS}
    -I "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/linux/init"
    -I "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/linux/netlinkutil"
    -ffunction-sections -fdata-sections)
set(LINUX_LDFLAGS ${LINUX_LDFLAGS} -Wl,--gc-sections -Wl,--wrap=socket,--wrap=bind,--wrap=nanosleep)

add_linux_executable(localhost_start_tests "${SOURCES}" "${PRODUCTION_HEADERS};${GENERATED_DIR}/Localization.h;${CMAKE_CURRENT_SOURCE_DIR}/../../../src/linux/init/localhost.cpp" "netlinkutil")
add_dependencies(localhost_start_tests localization)
add_dependencies(wsltests localhost_start_tests)
