set(SOURCES
    DistributionRegistration.cpp
    LxssUserCallback.cpp
    LxssUserSession.cpp
    LxssUserSessionFactory.cpp
    LxssIptables.cpp
    LxssHttpProxy.cpp
    LxssInstance.cpp
    PluginManager.cpp
    ServiceMain.cpp
    BridgedNetworking.cpp
    GnsRpcServer.cpp
    GuestTelemetryLogger.cpp
    Lifetime.cpp
    LxssConsoleManager.cpp
    LxssCreateProcess.cpp
    MirroredNetworking.cpp
    WslCoreGuestNetworkService.cpp
    WslCoreInstance.cpp
    WslMirroredNetworking.cpp
    WslCoreTcpIpStateTracking.cpp
    WslCoreVm.cpp
    main.rc
    ${CMAKE_CURRENT_BINARY_DIR}/../mc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/wsleventschema.rc
    application.manifest)

set(HEADERS
    ../../inc/comservicehelper.h
    DistributionRegistration.h
    LxssUserCallback.h
    LxssUserSession.h
    LxssUserSessionFactory.h
    LxssIptables.h
    LxssHttpProxy.h
    PluginManager.h
    LxssInstance.h
    BridgedNetworking.h
    GnsRpcServer.h
    GuestTelemetryLogger.h
    IMirroredNetworkManager.h
    Lifetime.h
    LxssConsoleManager.h
    LxssCreateProcess.h
    MirroredNetworking.h
    WslCoreGuestNetworkService.h
    WslCoreInstance.h
    WslMirroredNetworking.h
    WslCoreNetworkEndpoint.h
    WslCoreTcpIpStateTracking.h
    WslCoreVm.h)

add_executable(wslservice ${SOURCES} ${HEADERS})
add_dependencies(wslservice wslserviceidl wslservicemc)
add_compile_definitions(__WRL_CLASSIC_COM__)
add_compile_definitions(__WRL_DISABLE_STATIC_INITIALIZE__)
add_compile_definitions(USE_COM_CONTEXT_DEF=1)
set_target_properties(wslservice PROPERTIES LINK_FLAGS "/merge:minATL=.rdata /include:__minATLObjMap_LxssUserSession_COM")
target_link_libraries(wslservice
                      ${COMMON_LINK_LIBRARIES}
                      computecore
                      common
                      configfile
                      legacy_stdio_definitions
                      VirtDisk.lib
                      Winhttp.lib
                      Synchronization.lib)

target_precompile_headers(wslservice REUSE_FROM common)
set_target_properties(wslservice PROPERTIES FOLDER windows)