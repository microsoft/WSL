cmake_minimum_required(VERSION 3.20)  # add this at the very top

set(SOURCES
    main.cpp
)

add_executable(wsladiag ${SOURCES})

target_link_libraries(wsladiag
    ${COMMON_LINK_LIBRARIES}
    common
)

# Only try to reuse precompiled headers if 'common' exists
if (TARGET common)
    target_precompile_headers(wsladiag REUSE_FROM common)
endif()

set_target_properties(wsladiag PROPERTIES FOLDER windows)
