set(SOURCES
    DllMain.cpp
    WslCoreConfigInterface.cpp)

add_library(libwsl SHARED ${SOURCES} libwsl.def)

target_include_directories(libwsl
    PUBLIC ${CMAKE_CURRENT_LIST_DIR})

set_target_properties(libwsl PROPERTIES FOLDER windows)
target_precompile_headers(libwsl REUSE_FROM common)

target_link_libraries(libwsl
                      ${COMMON_LINK_LIBRARIES}
                      common
                      configfile
                      legacy_stdio_definitions
                      VirtDisk.lib)