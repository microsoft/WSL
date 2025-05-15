set(SOURCES
    main.cpp
    main.rc)

set(HEADERS
    resource.h
    )

add_executable(wslg WIN32 ${SOURCES} ${HEADERS})
add_dependencies(wslg
                 common)

target_link_libraries(wslg
                      ${COMMON_LINK_LIBRARIES}
                      common)

target_precompile_headers(wslg REUSE_FROM common)
set_target_properties(wslg PROPERTIES FOLDER windows)