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
    WSLCSessionManager.cpp
    WSLCSessionManagerFactory.cpp
    WSLCPluginNotifier.cpp
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
    WSLCSessionManager.h
    WSLCSessionManagerFactory.h
    WSLCPluginNotifier.h)

if (INCLUDE_OPENVMM)
    list(APPEND SOURCES
        OpenVmmVirtualMachine.cpp
        OpenVmmGrpcClient.cpp)
    list(APPEND HEADERS
        OpenVmmVirtualMachine.h
        OpenVmmGrpcClient.h)

    set(GRPC_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/grpc")
    set(GRPC_VM_SERVICE_PROTO "${WSL_DEVICE_HOST_SOURCE_DIR}/proto/VMService.proto")
    set(GRPC_VM_SERVICE_SOURCES
        "${GRPC_GENERATED_DIR}/VMService.pb.cc"
        "${GRPC_GENERATED_DIR}/VMService.grpc.pb.cc")
    set(GRPC_VM_SERVICE_HEADERS
        "${GRPC_GENERATED_DIR}/VMService.pb.h"
        "${GRPC_GENERATED_DIR}/VMService.grpc.pb.h")

    add_custom_command(
        OUTPUT ${GRPC_VM_SERVICE_SOURCES} ${GRPC_VM_SERVICE_HEADERS}
        COMMAND "${GRPC_INSTALL_DIR}/bin/protoc.exe"
            --proto_path=${WSL_DEVICE_HOST_SOURCE_DIR}/proto
            --proto_path=${GRPC_INSTALL_DIR}/include
            --cpp_out=${GRPC_GENERATED_DIR}
            --grpc_out=${GRPC_GENERATED_DIR}
            --plugin=protoc-gen-grpc=${GRPC_INSTALL_DIR}/bin/grpc_cpp_plugin.exe
                "${GRPC_VM_SERVICE_PROTO}"
        DEPENDS "${GRPC_VM_SERVICE_PROTO}"
        VERBATIM)

    list(APPEND SOURCES ${GRPC_VM_SERVICE_SOURCES})
    list(APPEND HEADERS ${GRPC_VM_SERVICE_HEADERS})
    set_source_files_properties(
        OpenVmmGrpcClient.cpp
        ${GRPC_VM_SERVICE_SOURCES}
        PROPERTIES COMPILE_OPTIONS "/wd4244;/wd4267;/wd4996")
endif()

add_executable(wslservice ${SOURCES} ${HEADERS})
add_dependencies(wslservice wslserviceidl wslservicemc)
target_compile_definitions(wslservice PRIVATE WSL_INCLUDE_OPENVMM=$<BOOL:${INCLUDE_OPENVMM}>)

add_compile_definitions(__WRL_CLASSIC_COM__)
add_compile_definitions(__WRL_DISABLE_STATIC_INITIALIZE__)
add_compile_definitions(USE_COM_CONTEXT_DEF=1)
set_target_properties(wslservice PROPERTIES LINK_FLAGS "/merge:minATL=.rdata /include:__minATLObjMap_LxssUserSession_COM /include:__minATLObjMap_WSLCSessionManager_COM")
target_link_libraries(wslservice
                      ${COMMON_LINK_LIBRARIES}
                      ${MSI_LINK_LIBRARIES}
                      ${HCS_LINK_LIBRARIES}
                      ${SERVICE_LINK_LIBRARIES}
                      common
                      configfile
                      legacy_stdio_definitions
                      VirtDisk.lib
                      Winhttp.lib
                      Synchronization.lib
                      yaml-cpp)

if (INCLUDE_OPENVMM)
    file(GLOB GRPC_NATIVE_LIBRARIES CONFIGURE_DEPENDS "${GRPC_INSTALL_DIR}/lib/*.lib")
    list(SORT GRPC_NATIVE_LIBRARIES ORDER DESCENDING)
    target_include_directories(wslservice PRIVATE "${GRPC_INSTALL_DIR}/include" "${GRPC_GENERATED_DIR}")
    target_compile_options(wslservice PRIVATE /wd4244 /wd4267 /wd4996)
    target_link_libraries(wslservice
                          ${GRPC_NATIVE_LIBRARIES}
                          Ws2_32.lib
                          Crypt32.lib
                          Advapi32.lib
                          Bcrypt.lib
                          Iphlpapi.lib
                          Userenv.lib)

endif()

target_precompile_headers(wslservice REUSE_FROM common)
set_target_properties(wslservice PROPERTIES FOLDER windows)
