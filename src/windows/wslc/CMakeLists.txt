set(SOURCES
    main.cpp
    ShellCommand.cpp
    ListCommand.cpp
    CreateCommand.cpp
    DeleteCommand.cpp
    StartCommand.cpp
    StopCommand.cpp
    ImageCommand.cpp
	ContainerCommand.cpp
    Utils.cpp
)

set(HEADERS
    ShellCommand.h
    ListCommand.h
    CreateCommand.h
    DeleteCommand.h
    StartCommand.h
    StopCommand.h
    ImageCommand.h
	ContainerCommand.h
    Utils.h
)

add_executable(wslc ${SOURCES} ${HEADERS})

target_link_libraries(wslc
    ${COMMON_LINK_LIBRARIES}
    common
)

target_precompile_headers(wslc REUSE_FROM common)

set_target_properties(wslc PROPERTIES FOLDER windows)