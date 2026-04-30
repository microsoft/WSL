set(SOURCES
    SimpleTests.cpp
    UnitTests.cpp
    MountTests.cpp
    NetworkTests.cpp
    Plan9Tests.cpp
    DrvFsTests.cpp
    Common.cpp
    PluginTests.cpp
    PolicyTests.cpp
    InstallerTests.cpp
    DiagnosticsTests.cpp)

set(HEADERS
    Common.h
    PluginTests.h
    lxsstest.h)

add_compile_definitions(INLINE_TEST_METHOD_MARKUP)

add_library(wsltests SHARED ${SOURCES} ${HEADERS})

target_include_directories(wsltests PRIVATE ${PROJECT_SOURCE_DIR}/src/windows/service/exe)
target_precompile_headers(wsltests REUSE_FROM common)
target_link_libraries(wsltests
                      common
                      ${TAEF_LINK_LIBRARIES}
                      ${COMMON_LINK_LIBRARIES}
                      ${MSI_LINK_LIBRARIES}
                      ${HCS_LINK_LIBRARIES}
                      ${SERVICE_LINK_LIBRARIES}
                      VirtDisk.lib
                      Wer.lib
                      Dbghelp.lib
                      sfc.lib)

add_dependencies(wsltests wslserviceidl)
add_subdirectory(testplugin)