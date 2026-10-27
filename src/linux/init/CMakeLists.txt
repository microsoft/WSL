set(SOURCES
    main.cpp
    binfmt.cpp
    config.cpp
    DnsServer.cpp
    DnsTunnelingChannel.cpp
    DnsTunnelingManager.cpp
    drvfs.cpp
    escape.cpp
    GnsEngine.cpp
    GnsPortTracker.cpp
    init.cpp
    localhost.cpp
    Localization.cpp
    NetworkManager.cpp
    plan9.cpp
    telemetry.cpp
    timezone.cpp
    SecCompDispatcher.cpp
    SecurityAuditLaunch.cpp
    util.cpp
    WslDistributionConfig.cpp
    wslinfo.cpp
    wslpath.cpp
    WSLCInit.cpp)

set(HEADERS
    ../inc/lxwil.h
    binfmt.h
    common.h
    config.h
    DnsServer.h
    DnsTunnelingChannel.h
    DnsTunnelingManager.h
    drvfs.h
    escape.h
    GnsEngine.h
    GnsPortTracker.h
    localhost.h
    NetworkManager.h
    plan9.h
    telemetry.h
    timezone.h
    SecCompDispatcher.h
    SecurityAuditLaunch.h
    util.h
    WslDistributionConfig.h
    wslinfo.h
    wslpath.h)

set(LINUX_CXXFLAGS ${LINUX_CXXFLAGS} -I "${CMAKE_CURRENT_LIST_DIR}/../netlinkutil")
set(LINUX_CXXFLAGS ${LINUX_CXXFLAGS} -I "${CMAKE_SOURCE_DIR}/src/shared/securityaudit")
set(INIT_LIBRARIES ${COMMON_LINUX_LINK_LIBRARIES} netlinkutil plan9 mountutil configfile securityaudit ebpf_loader bpf elf eu z)
add_linux_executable(init "${SOURCES}" "${HEADERS};${COMMON_LINUX_HEADERS}" "${INIT_LIBRARIES}")
add_dependencies(init localization)

set_target_properties(init PROPERTIES FOLDER linux)

set(INITRAMFS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/initrd.img)
set(INIT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/init)
add_custom_command(
    OUTPUT ${INITRAMFS} "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/initramfs"
    DEPENDS init ${INIT} wsl_audit_payload
        "${WSL_SECURITYAUDIT_STAGED_PAYLOAD}/audit-payload.json"
        "${CMAKE_SOURCE_DIR}/tools/create-initrd.ps1"
        "${CMAKE_SOURCE_DIR}/tools/securityaudit/Validate-Payload.ps1"
    COMMAND powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive
        -File "${CMAKE_SOURCE_DIR}/tools/create-initrd.ps1" "${INIT}" "${INITRAMFS}"
        -AuditPayloadDirectory "${WSL_SECURITYAUDIT_STAGED_PAYLOAD}" -Architecture "${LLVM_ARCH}"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/initramfs"
    VERBATIM)
add_custom_target(initramfs DEPENDS ${INITRAMFS})
set_target_properties(initramfs PROPERTIES FOLDER linux)
