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
    arguments/ArgumentValidation.h
    core/Command.h
    core/Core.h
    core/CLIExecutionContext.h
    core/ExecutionContextData.h
    core/Invocation.h
    commands/RootCommand.h
    commands/DiagCommand.h
    commands/ContainerCommand.h
    commands/ImageCommand.h
    commands/RegistryCommand.h
    commands/SessionCommand.h
    commands/VolumeCommand.h
    tasks/TaskBase.h
    tasks/CommonTasks.h
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
    commands/DiagAttachCommand.cpp
    commands/DiagBuildCommand.cpp
    commands/DiagListCommand.cpp
    commands/DiagLogsCommand.cpp
    commands/DiagPullCommand.cpp
    commands/DiagRunCommand.cpp
    commands/DiagShellCommand.cpp
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
    commands/RegistryCommand.cpp
    commands/RegistryLoginCommand.cpp
    commands/RegistryLogoutCommand.cpp
    commands/SessionCommand.cpp
    commands/SessionListCommand.cpp
    commands/VolumeCommand.cpp
    commands/VolumeCreateCommand.cpp
    commands/VolumeDeleteCommand.cpp
    commands/VolumeInspectCommand.cpp
    commands/VolumeListCommand.cpp
    commands/VolumePruneCommand.cpp
    tasks/TaskBase.cpp
    tasks/CommonTasks.cpp
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
