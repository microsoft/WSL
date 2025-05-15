set(SOURCES
    DllMain.cpp
    version.rc)

add_library(wslinstall SHARED ${SOURCES} wslinstall.def)

target_include_directories(wslinstall PUBLIC ${CMAKE_CURRENT_LIST_DIR})
set_target_properties(wslinstall PROPERTIES FOLDER windows)
target_precompile_headers(wslinstall REUSE_FROM common)

target_link_libraries(wslinstall
                      ${COMMON_LINK_LIBRARIES}
                      common
                      legacy_stdio_definitions
                      Msi.lib
                      Crypt32.lib
                      sfc.lib)