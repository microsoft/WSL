set(SOURCES
    application.manifest
    ContainerEventTracker.cpp
    DockerHTTPClient.cpp
    main.rc
    ServiceMain.cpp
    ServiceProcessLauncher.cpp
    WSLAContainer.cpp
    WSLAProcess.cpp
    WSLAProcessControl.cpp
    WSLAProcessIO.cpp
    WSLASession.cpp
    WSLASessionManager.cpp
    WSLASessionManagerFactory.cpp
    WSLAVirtualMachine.cpp
    )

set(HEADERS
    ContainerEventTracker.h
    DockerHTTPClient.h
    ServiceProcessLauncher.h
    WSLAContainer.h
    WSLAProcess.h
    WSLAProcessControl.h
    WSLAProcessIO.h
    WSLASession.h
    WSLASessionManager.h
    WSLASessionManagerFactory.h
    WSLAVirtualMachine.h)

include_directories(${CMAKE_SOURCE_DIR}/src/windows/wslaclient)

add_executable(wslaservice ${SOURCES} ${HEADERS})
add_dependencies(wslaservice wslaserviceidl)
add_compile_definitions(__WRL_CLASSIC_COM__)
add_compile_definitions(__WRL_DISABLE_STATIC_INITIALIZE__)
add_compile_definitions(USE_COM_CONTEXT_DEF=1)
set_target_properties(wslaservice PROPERTIES LINK_FLAGS "/merge:minATL=.rdata /include:__minATLObjMap_WSLAUserSession_COM")
target_link_libraries(wslaservice
                      ${COMMON_LINK_LIBRARIES}
                      computecore
                      common
                      configfile
                      legacy_stdio_definitions
                      VirtDisk.lib
                      Winhttp.lib
                      Synchronization.lib)

target_precompile_headers(wslaservice REUSE_FROM common)
set_target_properties(wslaservice PROPERTIES FOLDER windows)
