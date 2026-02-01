
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/commands
)

set(HEADERS
    inc/pch.h
    inc/argument.h
    inc/command.h
    inc/context.h
    inc/core.h
    inc/errors.h
    inc/executionargs.h
    inc/invocation.h
    commands/RootCommand.h
    commands/TestCommand.h
)

set(SOURCES
    argument.cpp
    command.cpp
    core.cpp
    main.cpp
    commands/RootCommand.cpp
    commands/TestCommand.cpp
)

add_executable(wslc2 ${HEADERS} ${SOURCES})

target_link_libraries(wslc2
    ${COMMON_LINK_LIBRARIES}
    common
)

target_precompile_headers(wslc2 REUSE_FROM common)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${HEADERS})

set_target_properties(wslc2 PROPERTIES FOLDER windows)