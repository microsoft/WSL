set(SOURCES
    main.cpp
    main.rc
    PluginHost.cpp)

set(HEADERS
    PluginHost.h
    resource.h)

add_executable(wslpluginhost WIN32 ${SOURCES} ${HEADERS})
add_dependencies(wslpluginhost
                 wslpluginhostidl
                 common)

target_include_directories(wslpluginhost PRIVATE
    ${CMAKE_BINARY_DIR}/src/windows/service/inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE})

target_link_libraries(wslpluginhost
                      ${COMMON_LINK_LIBRARIES}
                      ${MSI_LINK_LIBRARIES}
                      common
                      ole32.lib)

target_precompile_headers(wslpluginhost REUSE_FROM common)
set_target_properties(wslpluginhost PROPERTIES FOLDER windows)
