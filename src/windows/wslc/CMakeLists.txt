set(SOURCES
    main.cpp
    RunCommand.cpp
    ShellCommand.cpp
    ListCommand.cpp
    CreateCommand.cpp
    DeleteCommand.cpp
    StartCommand.cpp
    StopCommand.cpp
    ImageCommand.cpp
    Utils.cpp
)

set(HEADERS
    RunCommand.h
    ShellCommand.h
    ListCommand.h
    CreateCommand.h
    DeleteCommand.h
    StartCommand.h
    StopCommand.h
    ImageCommand.h
    Utils.h
)

add_executable(wslc ${SOURCES} ${HEADERS})

target_link_libraries(wslc
    ${COMMON_LINK_LIBRARIES}
    common
)

target_precompile_headers(wslc REUSE_FROM common)

set_target_properties(wslc PROPERTIES FOLDER windows)