set(SOURCES
    main.cpp
)

add_executable(wslc ${SOURCES})

target_link_libraries(wslc
    ${COMMON_LINK_LIBRARIES}
    common
)

target_precompile_headers(wslc REUSE_FROM common)

set_target_properties(wslc PROPERTIES FOLDER windows)