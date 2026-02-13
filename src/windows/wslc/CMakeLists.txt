include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/commands
    ${CMAKE_CURRENT_SOURCE_DIR}/arguments
    ${CMAKE_CURRENT_SOURCE_DIR}/tasks
)

set(HEADERS
    core/pch.h
    core/Errors.h
    core/Exceptions.h
    core/EnumVariantMap.h
    arguments/ArgumentDefinitions.h
    arguments/ArgumentTypes.h
    arguments/Argument.h
    arguments/ArgumentParser.h
    core/Command.h
    core/Core.h
    core/CLIExecutionContext.h
    core/ExecutionContextData.h
    core/Invocation.h
    commands/RootCommand.h
    commands/DiagCommand.h
    tasks/TaskBase.h
    tasks/DiagTasks.h
)

set(SOURCES
    core/Command.cpp
    core/CLIExecutionContext.cpp
    core/Core.cpp
    core/Main.cpp
    arguments/Argument.cpp
    arguments/ArgumentParser.cpp
    arguments/ArgumentValidation.cpp
    commands/RootCommand.cpp
    commands/DiagCommand.cpp
    commands/DiagListCommand.cpp
    tasks/TaskBase.cpp
    tasks/DiagTasks.cpp
)

# Object library for WSLC components.
# Used to build the executable and also unit testing components.
add_library(wslc OBJECT ${SOURCES} ${HEADERS})
set_target_properties(wslc PROPERTIES FOLDER windows)
target_include_directories(wslc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/commands
    ${CMAKE_CURRENT_SOURCE_DIR}/arguments
    ${CMAKE_CURRENT_SOURCE_DIR}/tasks
)
target_precompile_headers(wslc REUSE_FROM common)
target_link_libraries(wslc
    PRIVATE
    ${COMMON_LINK_LIBRARIES}
    common
)

# The WSLC executable
add_executable(wslccli $<TARGET_OBJECTS:wslc>)
target_link_libraries(wslccli
    PRIVATE
    ${COMMON_LINK_LIBRARIES}
    common
)
set_target_properties(wslccli PROPERTIES FOLDER windows)
set_target_properties(wslccli PROPERTIES OUTPUT_NAME "wslc")

# For prettier source tree browsing
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${HEADERS})
