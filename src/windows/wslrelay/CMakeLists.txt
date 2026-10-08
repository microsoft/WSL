set(SOURCES
    localhost.cpp
    main.cpp
    main.rc)

set(HEADERS
    localhost.h
    resource.h)

add_executable(wslrelay WIN32 ${SOURCES} ${HEADERS})
add_dependencies(wslrelay
                 common)

target_link_libraries(wslrelay
                      ${COMMON_LINK_LIBRARIES}
                      common)

target_precompile_headers(wslrelay REUSE_FROM common)
set_target_properties(wslrelay PROPERTIES FOLDER windows)