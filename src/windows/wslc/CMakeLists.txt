set(SOURCES
    main.cpp

    # Commands
    ShellCommand.cpp
    ListCommand.cpp
    ImageCommand.cpp
    ContainerCommand.cpp
	ICommand.cpp

    # Others
    Utils.cpp

    # Services
    ContainerService.cpp
	ImageService.cpp
)

set(HEADERS
    # Commands
    ShellCommand.h
    ListCommand.h
    ImageCommand.h
    ContainerCommand.h
	ICommand.h

    # Others
    Utils.h
	TableOutput.h

    # Services
    ContainerService.h
	ImageService.h
)

add_executable(wslc ${SOURCES} ${HEADERS})

target_link_libraries(wslc
    ${COMMON_LINK_LIBRARIES}
    common
)

target_precompile_headers(wslc REUSE_FROM common)

set_target_properties(wslc PROPERTIES FOLDER windows)