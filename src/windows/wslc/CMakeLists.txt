include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/commands
    ${CMAKE_CURRENT_SOURCE_DIR}/arguments
    ${CMAKE_CURRENT_SOURCE_DIR}/services
    ${CMAKE_CURRENT_SOURCE_DIR}/tasks
)

set(HEADERS
    core/Exceptions.h
    core/EnumVariantMap.h
    arguments/ArgumentDefinitions.h
    arguments/ArgumentTypes.h
    arguments/Argument.h
    arguments/ArgumentParser.h
    arguments/ArgumentValidation.h
    core/Command.h
    core/CLIExecutionContext.h
    core/ExecutionContextData.h
    core/Invocation.h
    core/TablePrinter.h
    commands/RootCommand.h
    commands/ContainerCommand.h
    commands/DiagCommand.h
    services/ConsoleService.h
    services/ContainerModel.h
    services/ContainerService.h
    services/ImageService.h
    services/PullImageCallback.h
    services/SessionModel.h
    services/SessionService.h
    tasks/Task.h
    tasks/DiagTasks.h
    tasks/ContainerTasks.h
)

set(SOURCES
    core/Command.cpp
    core/Main.cpp
    arguments/Argument.cpp
    arguments/ArgumentParser.cpp
    arguments/ArgumentValidation.cpp
    commands/RootCommand.cpp
    commands/ContainerCommand.cpp
    commands/ContainerCreateCommand.cpp
    commands/ContainerListCommand.cpp
    commands/ContainerRunCommand.cpp
    commands/ContainerStartCommand.cpp
    commands/ContainerStopCommand.cpp
    commands/DiagCommand.cpp
    commands/DiagListCommand.cpp
    services/ConsoleService.cpp
    services/ContainerService.cpp
    services/ImageService.cpp
    services/PullImageCallback.cpp
    services/SessionModel.cpp
    services/SessionService.cpp
    tasks/DiagTasks.cpp
    tasks/ContainerTasks.cpp
)

# Object library for WSLC components.
# Used to build the executable and also unit testing components.
add_library(wslclib OBJECT ${SOURCES} ${HEADERS})
set_target_properties(wslclib PROPERTIES
#    UNITY_BUILD ON
#    UNITY_BUILD_BATCH_SIZE 0  # 0 means CMake decides automatically
    FOLDER windows
)
target_include_directories(wslclib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/commands
    ${CMAKE_CURRENT_SOURCE_DIR}/arguments
    ${CMAKE_CURRENT_SOURCE_DIR}/services
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
