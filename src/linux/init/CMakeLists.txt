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
    securityaudit/ProcessExecutionAudit.cpp
    SecurityAudit.cpp
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
    securityaudit/ProcessExecutionAudit.h
    util.h
    WslDistributionConfig.h
    wslinfo.h
    wslpath.h)

set(AUDIT_PAYLOAD_DIRECTORY "${SECURITYAUDIT_SOURCE_DIR}/include/ebpf")
list(APPEND HEADERS
    "${AUDIT_PAYLOAD_DIRECTORY}/event_defs.h"
    "${AUDIT_PAYLOAD_DIRECTORY}/ebpf_plugin_loader.h"
    "${AUDIT_PAYLOAD_DIRECTORY}/ebpf_arch_macros.h")
set(LINUX_CXXFLAGS ${LINUX_CXXFLAGS}
    -I "${CMAKE_CURRENT_LIST_DIR}"
    -I "${CMAKE_CURRENT_LIST_DIR}/../netlinkutil"
    -I "${AUDIT_PAYLOAD_DIRECTORY}")
set(INIT_LIBRARIES ${COMMON_LINUX_LINK_LIBRARIES} netlinkutil plan9 mountutil configfile)
foreach(library ebpf_loader bpf elf eu z)
    list(APPEND INIT_LIBRARIES "${SECURITYAUDIT_SOURCE_DIR}/runtimes/linux-musl-${TARGET_PLATFORM}/native/lib${library}.a")
endforeach()
add_linux_executable(init "${SOURCES}" "${HEADERS};${COMMON_LINUX_HEADERS}" "${INIT_LIBRARIES}")
add_dependencies(init localization)

set_target_properties(init PROPERTIES FOLDER linux)

set(INITRAMFS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/initrd.img)
set(INIT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/init)
file(GLOB AUDIT_PAYLOAD_FILES CONFIGURE_DEPENDS "${AUDIT_PAYLOAD_DIRECTORY}/*.o" "${AUDIT_PAYLOAD_DIRECTORY}/*.conf")
add_custom_command(
    OUTPUT ${INITRAMFS} "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/initramfs"
    DEPENDS init ${INIT} ${AUDIT_PAYLOAD_FILES} "${CMAKE_SOURCE_DIR}/tools/create-initrd.ps1"
    COMMAND powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive -File "${CMAKE_SOURCE_DIR}/tools/create-initrd.ps1" "${INIT}" "${INITRAMFS}"
        -AuditPayloadDirectory "${AUDIT_PAYLOAD_DIRECTORY}" -Architecture "${LLVM_ARCH}"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/initramfs"
    VERBATIM)
add_custom_target(initramfs DEPENDS ${INITRAMFS})
set_target_properties(initramfs PROPERTIES FOLDER linux)
