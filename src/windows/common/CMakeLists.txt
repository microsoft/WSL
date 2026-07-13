set(SOURCES
    APICompat.cpp
    ConsoleProgressBar.cpp
    ConsoleProgressIndicator.cpp
    ConsoleState.cpp
    DeviceHostProxy.cpp
    disk.cpp
    Distribution.cpp
    Dmesg.cpp
    DnsResolver.cpp
    DnsTunnelingChannel.cpp
    ExecutionContext.cpp
    filesystem.cpp
    GnsChannel.cpp
    GnsPortTrackerChannel.cpp
    GuestDeviceManager.cpp
    HandleConsoleProgressBar.cpp
    hcs.cpp
    helpers.cpp
    hvsocket.cpp
    interop.cpp
    HandleIO.cpp
    Localization.cpp
    lxssbusclient.cpp
    lxssclient.cpp
    LxssMessagePort.cpp
    LxssSecurity.cpp
    LxssServerPort.cpp
    NatNetworking.cpp
    notifications.cpp
    Redirector.cpp
    registry.cpp
    relay.cpp
    RingBuffer.cpp
    socket.cpp
    string.cpp
    SubProcess.cpp
    svccomm.cpp
    VTSupport.cpp
    WindowsUpdateIntegration.cpp
    WSLCContainerLauncher.cpp
    ConsommeNetworking.cpp
    WSLCProcessLauncher.cpp
    WslClient.cpp
    WslCoreConfig.cpp
    WslCoreFilesystem.cpp
    WslCoreFirewallSupport.cpp
    WslCoreHostDnsInfo.cpp
    WslCoreNetworkEndpointSettings.cpp
    WslCoreNetworkingSupport.cpp
    WslInstall.cpp
    WslSecurity.cpp
    SlowOperationWatcher.cpp
    WslTelemetry.cpp
    wslutil.cpp
    install.cpp
    WSLCUserSettings.cpp
    notifications.cpp)

set(HEADERS
    ../../../generated/Localization.h
    APICompat.h
    ../inc/docker_schema.h
    ../inc/wslc_schema.h
    ../inc/lxssbusclient.h
    ../inc/lxssclient.h
    ../inc/LxssDynamicFunction.h
    ../inc/traceloggingconfig.h
    ../inc/wdk.h
    ../inc/wsl.h
    ../inc/wslconfig.h
    ../inc/wslhost.h
    ../inc/wslpolicies.h
    ../inc/WslPluginApi.h
    ../inc/wslrelay.h
    ../../shared/inc/CommandLine.h
    ../../shared/inc/defs.h
    ../../shared/inc/hns_schema.h
    ../../shared/inc/JsonUtils.h
    ../../shared/inc/lxfsshares.h
    ../../shared/inc/lxinitshared.h
    ../../shared/inc/message.h
    ../../shared/inc/prettyprintshared.h
    ../../shared/inc/retryshared.h
    ../../shared/inc/SocketChannel.h
    ../../shared/inc/socketshared.h
    ../../shared/inc/stringshared.h
    ConsoleProgressBar.h
    ConsoleProgressIndicator.h
    ConsoleState.h
    DeviceHostProxy.h
    disk.hpp
    Distribution.h
    Dmesg.h
    DnsResolver.h
    DnsTunnelingChannel.h
    ExecutionContext.h
    filesystem.hpp
    GnsChannel.h
    GnsPortTrackerChannel.h
    GuestDeviceManager.h
    HandleConsoleProgressBar.h
    hcs.hpp
    hcs_schema.h
    helpers.hpp
    hvsocket.hpp
    INetworkingEngine.h
    interop.hpp
    HandleIO.h
    LxssMessagePort.h
    LxssPort.h
    LxssSecurity.h
    LxssServerPort.h
    NatNetworking.h
    notifications.h
    precomp.h
    Redirector.h
    registry.hpp
    relay.hpp
    RingBuffer.h
    socket.hpp
    string.hpp
    Stringify.h
    SubProcess.h
    svccomm.hpp
    VTSupport.h
    WindowsUpdateIntegration.h
    WSLCContainerLauncher.h
    ConsommeNetworking.h
    WSLCProcessLauncher.h
    WslClient.h
    WslCoreConfig.h
    WslCoreFilesystem.h
    WslCoreFirewallSupport.h
    WslCoreHostDnsInfo.h
    WslCoreMessageQueue.h
    WslCoreNetworkEndpointSettings.h
    WslCoreNetworkingSupport.h
    WslInstall.h
    WslSecurity.h
    SlowOperationWatcher.h
    WslTelemetry.h
    wslutil.h
    EnumVariantMap.h
    WSLCUserSettings.h
    WSLCSessionDefaults.h
    )

# Adds a target for the common static library.
# This is factored into a function so that we can have multiple variants with different options.
function(add_common_library TARGET_NAME)
    add_library(${TARGET_NAME} STATIC ${SOURCES} ${HEADERS})
    add_dependencies(${TARGET_NAME} wslserviceidl localization wslservicemc wslinstalleridl yaml-cpp)
    target_precompile_headers(${TARGET_NAME} PRIVATE precomp.h)
    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER windows)
    target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/../service/mc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE})
endfunction()

add_common_library(common)

# WSLCUserSettings.cpp uses yaml-cpp headers.
set_source_files_properties(WSLCUserSettings.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${yaml-cpp_SOURCE_DIR}/include"
    COMPILE_DEFINITIONS "YAML_CPP_STATIC_DEFINE")

# For Fuzzing builds, there are some targets that cannot be built with ASAN instrumentation.
# These targets use a non-instrumented copy of the common library.
if (WSL_BUILD_FUZZING)
    # Remove ASAN from all targets in this directory, then add it back only for the base variant.
    string(REPLACE "/fsanitize=address" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    target_compile_options(common PRIVATE /fsanitize=address)

    add_common_library(common_noasan)
endif()
