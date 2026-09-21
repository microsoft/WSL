set(SOURCES
    common/Common.cpp)

set(HEADERS
    common/Common.h
    common/lxsstest.h)

add_compile_definitions(INLINE_TEST_METHOD_MARKUP)

add_library(wsltests SHARED ${SOURCES} ${HEADERS})

target_include_directories(wsltests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/src/linux/plan9
    ${CMAKE_SOURCE_DIR}/src/windows/WslcSDK
    ${CMAKE_BINARY_DIR}/src/windows/WslcSDK/winrt/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE})
set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/wsl/e2e/NetworkTests.cpp APPEND PROPERTY INCLUDE_DIRECTORIES
    ${CMAKE_SOURCE_DIR}/src/windows/service/exe)
target_link_directories(wsltests PRIVATE ${BIN})
target_precompile_headers(wsltests REUSE_FROM common)
target_link_libraries(wsltests
                      common
                      wslclib
                      wslcsdk
                      ${TAEF_LINK_LIBRARIES}
                      ${COMMON_LINK_LIBRARIES}
                      ${MSI_LINK_LIBRARIES}
                      ${HCS_LINK_LIBRARIES}
                      yaml-cpp
                      ${SERVICE_LINK_LIBRARIES}
                      VirtDisk.lib
                      Wer.lib
                      Dbghelp.lib
                      sfc.lib
                      Crypt32.lib
                      Ncrypt.lib)

add_dependencies(wsltests wslserviceidl wslclib wslc wslcsdk wslcsdkwinrtidl)
add_subdirectory(testplugin)
add_subdirectory(wsl)
add_subdirectory(wslc)

# For prettier source tree browsing
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${HEADERS})
