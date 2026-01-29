set(SOURCES
    main.cpp

    # Commands
    ShellCommand.cpp
    ListCommand.cpp
    CreateCommand.cpp
    DeleteCommand.cpp
    StartCommand.cpp
    StopCommand.cpp
    ImageCommand.cpp
    ContainerCommand.cpp

    # Others
    Utils.cpp

    # Services
    services/ContainerService.cpp
	services/ImageService.cpp
)

set(HEADERS
    # Commands
    ShellCommand.h
    ListCommand.h
    CreateCommand.h
    DeleteCommand.h
    StartCommand.h
    StopCommand.h
    ImageCommand.h
    ContainerCommand.h

    # Others
    Utils.h
	TableOutput.h

    # Services
    services/ContainerService.h
	services/ImageService.h
)

add_executable(wslc ${SOURCES} ${HEADERS})

target_link_libraries(wslc
    ${COMMON_LINK_LIBRARIES}
    common
)

target_precompile_headers(wslc REUSE_FROM common)

set_target_properties(wslc PROPERTIES FOLDER windows)