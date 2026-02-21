set(SOURCES
    main.cpp
    main.rc
    application.manifest

    # Session factory and service reference
    WSLASessionFactory.cpp
    WSLASessionReference.cpp

    # Session and container implementation
    WSLASession.cpp
    WSLAContainer.cpp
    WSLAVirtualMachine.cpp

    # Process management
    WSLAProcess.cpp
    WSLAProcessControl.cpp
    WSLAProcessIO.cpp

    # Supporting classes
    ContainerEventTracker.cpp
    DockerHTTPClient.cpp
    IORelay.cpp
    ServiceProcessLauncher.cpp
    )

set(HEADERS
    ContainerEventTracker.h
    DockerHTTPClient.h
    IORelay.h
    ServiceProcessLauncher.h
    WSLAContainer.h
    WSLAContainerMetadata.h
    WSLAProcess.h
    WSLAProcessControl.h
    WSLAProcessIO.h
    WSLASession.h
    WSLASessionFactory.h
    WSLASessionReference.h
    WSLAVirtualMachine.h)

add_executable(wslasession WIN32 ${SOURCES} ${HEADERS})
add_dependencies(wslasession wslaserviceidl)
add_compile_definitions(__WRL_CLASSIC_COM__)
add_compile_definitions(USE_COM_CONTEXT_DEF=1)
target_link_libraries(wslasession
                      ${COMMON_LINK_LIBRARIES}
                      common
                      legacy_stdio_definitions
                      VirtDisk.lib
                      Dbghelp.lib)

target_precompile_headers(wslasession REUSE_FROM common)
set_target_properties(wslasession PROPERTIES FOLDER windows)
