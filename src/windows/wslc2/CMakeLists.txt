include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/foundation
    ${CMAKE_CURRENT_SOURCE_DIR}/commands
    ${CMAKE_CURRENT_SOURCE_DIR}/arguments
    ${CMAKE_CURRENT_SOURCE_DIR}/tasks
    ${CMAKE_CURRENT_SOURCE_DIR}/services
)

set(HEADERS

    core/pch.h
    core/foundation/DateTime.h
    core/foundation/Errors.h
    core/foundation/Exceptions.h
    core/foundation/EnumVariantMap.h
    core/foundation/SharedThreadGlobals.h
    core/foundation/Logging.h
    core/foundation/SystemUtilities.h
    core/foundation/StringUtilities.h
    core/foundation/DebugLogger.h
    core/foundation/FileLogger.h
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
    commands/TestCommand.h
    commands/ContainerCommand.h
    commands/ImageCommand.h
    tasks/TaskBase.h
    tasks/CommonTasks.h
    tasks/TestTasks.h
    services/ContainerService.h
)

set(SOURCES
    core/foundation/DateTime.cpp
    core/foundation/SystemUtilities.cpp
    core/foundation/StringUtilities.cpp
    core/foundation/SharedThreadGlobals.cpp
    core/foundation/Logging.cpp
    core/foundation/DebugLogger.cpp
    core/foundation/FileLogger.cpp
    core/Command.cpp
    core/CLIExecutionContext.cpp
    core/Core.cpp
    core/Main.cpp
    arguments/Argument.cpp
    arguments/ArgumentParser.cpp
    arguments/ArgumentValidation.cpp
    arguments/ContainerArgumentValidation.cpp
    commands/RootCommand.cpp
    commands/TestCommand.cpp
    commands/ContainerCommand.cpp
    commands/ContainerCreateCommand.cpp
    commands/ContainerDeleteCommand.cpp
    commands/ContainerExecCommand.cpp
    commands/ContainerInspectCommand.cpp
    commands/ContainerKillCommand.cpp
    commands/ContainerListCommand.cpp
    commands/ContainerRunCommand.cpp
    commands/ContainerStartCommand.cpp
    commands/ContainerStopCommand.cpp
    commands/ImageCommand.cpp
    commands/ImageInspectCommand.cpp
    commands/ImageListCommand.cpp
    commands/ImageLoadCommand.cpp
    commands/ImagePruneCommand.cpp
    commands/ImagePullCommand.cpp
    commands/ImagePushCommand.cpp
    commands/ImageSaveCommand.cpp
    commands/ImageTagCommand.cpp
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