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
    commands/TestCommand.h
    commands/ContainerCommand.h
    commands/ImageCommand.h
    commands/RegistryCommand.h
    commands/SessionCommand.h
    commands/VolumeCommand.h
    tasks/TaskBase.h
    tasks/CommonTasks.h
)

set(SOURCES
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
)

# Object library for WSLC components.
# Used to build the executable and also unit testing components.
add_library(wslc2 OBJECT ${SOURCES} ${HEADERS})
set_target_properties(wslc2 PROPERTIES FOLDER windows)
target_include_directories(wslc2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/commands
    ${CMAKE_CURRENT_SOURCE_DIR}/arguments
    ${CMAKE_CURRENT_SOURCE_DIR}/tasks
)
target_precompile_headers(wslc2 REUSE_FROM common)
target_link_libraries(wslc2
    PRIVATE
    ${COMMON_LINK_LIBRARIES}
    common
)

# The WSLC executable
add_executable(wslccli $<TARGET_OBJECTS:wslc2>)
target_link_libraries(wslccli
    PRIVATE
    ${COMMON_LINK_LIBRARIES}
    common
)
set_target_properties(wslccli PROPERTIES FOLDER windows)
set_target_properties(wslccli PROPERTIES OUTPUT_NAME "wslc2")

# For prettier source tree browsing
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${HEADERS})
