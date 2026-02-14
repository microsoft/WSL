set(SOURCES
    main.cpp

    # Commands
    ShellCommand.cpp
    ImageCommand.cpp
    ContainerCommand.cpp
    ICommand.cpp

    # Services
    ContainerService.cpp
    ImageService.cpp
    ShellService.cpp
    SessionService.cpp
    ConsoleService.cpp

    # Models
    SessionModel.cpp
)

set(HEADERS
    # Commands
    ShellCommand.h
    ImageCommand.h
    ContainerCommand.h
    ICommand.h

    # Others
    TablePrinter.h

    # Services
    ContainerService.h
    ImageService.h
    ShellService.h
    SessionService.h
    ConsoleService.h

    # Models
    ImageModel.h
    ContainerModel.h
    SessionModel.h
)

source_group("Services" FILES
    ContainerService.h
    ContainerService.cpp
    ImageService.h
    ImageService.cpp
    ShellService.h
    ShellService.cpp
    SessionService.h
    SessionService.cpp
    ConsoleService.h
    ConsoleService.cpp
)

source_group("Commands" FILES
    ShellCommand.h
    ShellCommand.cpp
    ImageCommand.h
    ImageCommand.cpp
    ContainerCommand.h
    ContainerCommand.cpp
    ICommand.h
    ICommand.cpp
)

source_group("Models" FILES
    ImageModel.h
    ContainerModel.h
    SessionModel.h
    SessionModel.cpp
)

add_executable(wslc ${SOURCES} ${HEADERS})

target_link_libraries(wslc
    ${COMMON_LINK_LIBRARIES}
    common
)

target_precompile_headers(wslc REUSE_FROM common)

set_target_properties(wslc PROPERTIES FOLDER windows)