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
    HcsVirtualMachine.cpp
    OpenVmmVirtualMachine.cpp
    TtrpcEnvelopeCodec.cpp
    TtrpcClient.cpp
    WSLCSessionManager.cpp
    WSLCSessionManagerFactory.cpp
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
    WslCoreVm.h
    HcsVirtualMachine.h
    OpenVmmVirtualMachine.h
    TtrpcEnvelopeCodec.h
    TtrpcClient.h
    WSLCSessionManager.h
    WSLCSessionManagerFactory.h)

set(WSLSERVICE_PROTO_FILE ${WSL_DEVICE_HOST_SOURCE_DIR}/proto/VMService.proto)
set(WSLSERVICE_PROTO_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(WSLSERVICE_GENERATED_PROTO_SRC ${WSLSERVICE_PROTO_OUTPUT_DIR}/VMService.pb.cc)
set(WSLSERVICE_GENERATED_PROTO_HDR ${WSLSERVICE_PROTO_OUTPUT_DIR}/VMService.pb.h)

if (NOT EXISTS ${WSLSERVICE_PROTO_FILE})
    message(FATAL_ERROR "Expected DeviceHost proto file was not found: ${WSLSERVICE_PROTO_FILE}")
endif()

add_custom_command(
    OUTPUT ${WSLSERVICE_GENERATED_PROTO_SRC} ${WSLSERVICE_GENERATED_PROTO_HDR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WSLSERVICE_PROTO_OUTPUT_DIR}
    COMMAND $<TARGET_FILE:${PROTOC_TARGET}>
            --cpp_out=${WSLSERVICE_PROTO_OUTPUT_DIR}
            --proto_path=${WSL_DEVICE_HOST_SOURCE_DIR}/proto
            --proto_path=${protobuf_SOURCE_DIR}/src
            ${WSLSERVICE_PROTO_FILE}
    DEPENDS ${WSLSERVICE_PROTO_FILE} ${PROTOC_TARGET}
    VERBATIM)

list(APPEND SOURCES ${WSLSERVICE_GENERATED_PROTO_SRC})
list(APPEND HEADERS ${WSLSERVICE_GENERATED_PROTO_HDR})

if (MSVC)
    set_source_files_properties(${WSLSERVICE_GENERATED_PROTO_SRC} PROPERTIES COMPILE_OPTIONS "/wd4267;/wd4244;/wd4018")
    set_source_files_properties(TtrpcClient.cpp PROPERTIES COMPILE_OPTIONS "/wd4267;/wd4244;/wd4018")
endif()

add_executable(wslservice ${SOURCES} ${HEADERS})
add_dependencies(wslservice wslserviceidl wslservicemc)
target_include_directories(wslservice PRIVATE ${WSLSERVICE_PROTO_OUTPUT_DIR})
add_compile_definitions(__WRL_CLASSIC_COM__)
add_compile_definitions(__WRL_DISABLE_STATIC_INITIALIZE__)
add_compile_definitions(USE_COM_CONTEXT_DEF=1)
set_target_properties(wslservice PROPERTIES LINK_FLAGS "/merge:minATL=.rdata /include:__minATLObjMap_LxssUserSession_COM /include:__minATLObjMap_WSLCSessionManager_COM")
target_link_libraries(wslservice
                      ${COMMON_LINK_LIBRARIES}
                      ${MSI_LINK_LIBRARIES}
                      ${HCS_LINK_LIBRARIES}
                      ${SERVICE_LINK_LIBRARIES}
                      ${PROTOBUF_LIB_TARGET}
                      common
                      configfile
                      legacy_stdio_definitions
                      VirtDisk.lib
                      Winhttp.lib
                      Synchronization.lib
                      yaml-cpp)

target_precompile_headers(wslservice REUSE_FROM common)
set_target_properties(wslservice PROPERTIES FOLDER windows)
