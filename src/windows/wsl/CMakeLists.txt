set(SOURCES
    main.cpp
    main.rc)

set(HEADERS
    resource.h)

add_executable(wsl ${SOURCES} ${HEADERS})

target_link_libraries(wsl
                      ${COMMON_LINK_LIBRARIES}
                      common
                      runtimeobject.lib)

target_precompile_headers(wsl REUSE_FROM common)
set_target_properties(wsl PROPERTIES FOLDER windows)