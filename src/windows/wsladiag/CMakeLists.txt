set(SOURCES
    main.cpp
)

add_executable(wsladiag ${SOURCES})

target_link_libraries(wsladiag
    ${COMMON_LINK_LIBRARIES}
    common
)

target_precompile_headers(wsladiag REUSE_FROM common)

set_target_properties(wsladiag PROPERTIES FOLDER windows)