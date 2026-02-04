
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/commands
    ${CMAKE_CURRENT_SOURCE_DIR}/tasks
    ${CMAKE_CURRENT_SOURCE_DIR}/services
)

set(HEADERS
    core/pch.h
    core/Util.h
    core/Argument.h
    core/Command.h
    core/Core.h
    core/Errors.h
    core/ExecutionArgs.h
    core/ExecutionContext.h
    core/ExecutionContextData.h
    core/ExecutionHelpers.h
    core/Invocation.h
    commands/RootCommand.h
    commands/TestCommand.h
    commands/ContainerCommand.h
    tasks/TaskBase.h
    tasks/CommonTasks.h
    tasks/TestTasks.h
    services/ContainerService.h
)

set(SOURCES
    core/Util.cpp
    core/Argument.cpp
    core/Command.cpp
    core/ExecutionContext.cpp
    core/Core.cpp
    core/Main.cpp
    commands/RootCommand.cpp
    commands/TestCommand.cpp
    commands/ContainerCommand.cpp
    tasks/TaskBase.cpp
    tasks/CommonTasks.cpp
    tasks/TestTasks.cpp
    services/ContainerService.cpp
)

add_executable(wslc2 ${HEADERS} ${SOURCES})

target_link_libraries(wslc2
    ${COMMON_LINK_LIBRARIES}
    common
)

target_precompile_headers(wslc2 REUSE_FROM common)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${HEADERS})

set_target_properties(wslc2 PROPERTIES FOLDER windows)