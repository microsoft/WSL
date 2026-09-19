set(SOURCES
    DllMain.cpp
    version.rc)

if (WSL_BUILD_FUZZING)
    # wslinstall is an MSI custom action DLL loaded from a temp directory at install time,
    # where the dynamic ASAN runtime can't be resolved. Build it without ASAN so it has no
    # dependency on the runtime. This also requires linking a non-instrumented common library.
    string(REPLACE "/fsanitize=address" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    set(WSLINSTALL_COMMON common_noasan)
else()
    set(WSLINSTALL_COMMON common)
endif()

add_library(wslinstall SHARED ${SOURCES} wslinstall.def)

target_include_directories(wslinstall PUBLIC ${CMAKE_CURRENT_LIST_DIR})
set_target_properties(wslinstall PROPERTIES FOLDER windows)
target_precompile_headers(wslinstall REUSE_FROM ${WSLINSTALL_COMMON})

target_link_libraries(wslinstall
                      ${COMMON_LINK_LIBRARIES}
                      ${MSI_LINK_LIBRARIES}
                      ${WSLINSTALL_COMMON}
                      legacy_stdio_definitions
                      Crypt32.lib
                      sfc.lib)