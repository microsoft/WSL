set(SOURCES
    main.cpp
    main.rc
    application.manifest

    # Session factory and service reference
    WSLCSessionFactory.cpp
    WSLCSessionReference.cpp

    # Session and container implementation
    WSLCSession.cpp
    WSLCContainer.cpp
    WSLCVirtualMachine.cpp

    # Process management
    WSLCProcess.cpp
    WSLCProcessControl.cpp
    WSLCProcessIO.cpp

    # Volume management
    WSLCVhdVolume.cpp
    WSLCGuestVolume.cpp
    WSLCVolumes.cpp

    # Supporting classes
    DockerEventTracker.cpp
    DockerHTTPClient.cpp
    IORelay.cpp
    ServiceProcessLauncher.cpp
    )

set(HEADERS
    DockerEventTracker.h
    DockerHTTPClient.h
    IORelay.h
    ServiceProcessLauncher.h
    WSLCContainer.h
    WSLCContainerMetadata.h
    WSLCProcess.h
    WSLCProcessControl.h
    WSLCProcessIO.h
    WSLCSession.h
    WSLCSessionFactory.h
    WSLCSessionReference.h
    WSLCVirtualMachine.h
    WSLCVhdVolume.h
    WSLCGuestVolume.h
    WSLCVolumes.h
    IWSLCVolume.h
    WSLCVolumeMetadata.h)

add_executable(wslcsession WIN32 ${SOURCES} ${HEADERS})
add_dependencies(wslcsession wslserviceidl)
add_compile_definitions(__WRL_CLASSIC_COM__)
add_compile_definitions(USE_COM_CONTEXT_DEF=1)
target_link_libraries(wslcsession
                      ${COMMON_LINK_LIBRARIES}
                      common
                      legacy_stdio_definitions
                      VirtDisk.lib
                      Crypt32.lib)

target_precompile_headers(wslcsession REUSE_FROM common)
set_target_properties(wslcsession PROPERTIES FOLDER windows)
