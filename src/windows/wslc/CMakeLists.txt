include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/commands
    ${CMAKE_CURRENT_SOURCE_DIR}/arguments
    ${CMAKE_CURRENT_SOURCE_DIR}/tasks
)

set(HEADERS
    core/Exceptions.h
    core/EnumVariantMap.h
    arguments/ArgumentDefinitions.h
    arguments/ArgumentTypes.h
    arguments/Argument.h
    arguments/ArgumentParser.h
    core/Command.h
    core/CLIExecutionContext.h
    core/ExecutionContextData.h
    core/Invocation.h
    commands/RootCommand.h
    commands/DiagCommand.h
    tasks/Task.h
    tasks/DiagTasks.h
)

set(SOURCES
    core/Command.cpp
    core/Main.cpp
    arguments/Argument.cpp
    arguments/ArgumentParser.cpp
    commands/RootCommand.cpp
    commands/DiagCommand.cpp
    commands/DiagListCommand.cpp
    tasks/DiagTasks.cpp
)

# Object library for WSLC components.
# Used to build the executable and also unit testing components.
add_library(wslclib OBJECT ${SOURCES} ${HEADERS})
set_target_properties(wslclib PROPERTIES FOLDER windows)
target_include_directories(wslclib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/commands
    ${CMAKE_CURRENT_SOURCE_DIR}/arguments
    ${CMAKE_CURRENT_SOURCE_DIR}/tasks
)
target_precompile_headers(wslclib REUSE_FROM common)
target_link_libraries(wslclib
    PRIVATE
    ${COMMON_LINK_LIBRARIES}
    common
)

# Create wslc.exe
add_executable(wslc $<TARGET_OBJECTS:wslclib>)
target_link_libraries(wslc
    PRIVATE
    ${COMMON_LINK_LIBRARIES}
    common
)
set_target_properties(wslc PROPERTIES FOLDER windows)

# For prettier source tree browsing
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${HEADERS})
