# Per-user COM server for WSLA sessions - runs under user identity
#
# TODO: Move these source files from wslaservice/exe into this directory.
# They are being left there for now to make this change easier to review.
#
set(SOURCES
    main.cpp
    main.rc
    application.manifest

    # Session factory and service reference
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLASessionFactory.cpp
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLASessionReference.cpp

    # Session and container implementation
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLASession.cpp
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLAContainer.cpp
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLAVirtualMachine.cpp

    # Process management
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLAProcess.cpp
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLAProcessControl.cpp
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLAProcessIO.cpp

    # Supporting classes
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/ContainerEventTracker.cpp
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/DockerHTTPClient.cpp
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/IORelay.cpp
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/ServiceProcessLauncher.cpp
    )

set(HEADERS
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/COMImplClass.h
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/ContainerEventTracker.h
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/DockerHTTPClient.h
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/IORelay.h
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/ServiceProcessLauncher.h
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLAContainer.h
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLAProcess.h
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLAProcessControl.h
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLAProcessIO.h
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLASession.h
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLASessionFactory.h
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLASessionReference.h
    ${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe/WSLAVirtualMachine.h)

include_directories(${CMAKE_SOURCE_DIR}/src/windows/wslaclient)
include_directories(${CMAKE_SOURCE_DIR}/src/windows/wslaservice/exe)

add_executable(wslasession WIN32 ${SOURCES} ${HEADERS})
add_dependencies(wslasession wslaserviceidl)
add_compile_definitions(__WRL_CLASSIC_COM__)
add_compile_definitions(USE_COM_CONTEXT_DEF=1)
target_link_libraries(wslasession
                      ${COMMON_LINK_LIBRARIES}
                      computecore
                      common
                      configfile
                      legacy_stdio_definitions
                      VirtDisk.lib
                      Winhttp.lib
                      Synchronization.lib)

target_precompile_headers(wslasession REUSE_FROM common)
set_target_properties(wslasession PROPERTIES FOLDER windows)
