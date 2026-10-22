# common/ holds the fixtures and helpers shared by the wsl/ and wslc/ tests, along
# with the unit tests for src/windows/common.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/common/*.cpp
)

file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/common/*.h
)

add_compile_definitions(INLINE_TEST_METHOD_MARKUP)

add_library(wsltests SHARED ${SOURCES} ${HEADERS})

target_include_directories(wsltests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/src/linux/plan9
    ${CMAKE_SOURCE_DIR}/src/windows/WslcSDK
    ${CMAKE_BINARY_DIR}/src/windows/WslcSDK/winrt/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE})
set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/wsl/NetworkTests.cpp APPEND PROPERTY INCLUDE_DIRECTORIES
    ${CMAKE_SOURCE_DIR}/src/windows/service/exe)
target_link_directories(wsltests PRIVATE ${BIN})
target_precompile_headers(wsltests REUSE_FROM common)
target_link_libraries(wsltests
                      common
                      wsl_securityaudit_windows
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

# Unit-only TAEF module: intentionally excludes common/Common.cpp and its global
# package/distro/.wslconfig setup. Run directly with the existing te.exe runner.
add_library(wslaudittests SHARED common/NativeAuditUnitTests.cpp)
target_include_directories(wslaudittests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_precompile_headers(wslaudittests REUSE_FROM common)
target_link_libraries(wslaudittests PRIVATE wsl_securityaudit_windows common
    ${TAEF_LINK_LIBRARIES} ${COMMON_LINK_LIBRARIES} ${MSI_LINK_LIBRARIES}
    ${HCS_LINK_LIBRARIES} ${SERVICE_LINK_LIBRARIES})
set_target_properties(wslaudittests PROPERTIES FOLDER tests)

# For prettier source tree browsing
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${HEADERS})
