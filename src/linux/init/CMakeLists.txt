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
    util.cpp
    WslDistributionConfig.cpp
    wslinfo.cpp
    wslpath.cpp)

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
    util.h
    WslDistributionConfig.h
    wslinfo.h
    wslpath.h)

set(LINUX_CXXFLAGS ${LINUX_CXXFLAGS} -I "${CMAKE_CURRENT_LIST_DIR}/../netlinkutil")
set(INIT_LIBRARIES ${COMMON_LINUX_LINK_LIBRARIES} netlinkutil plan9 mountutil configfile)
add_linux_executable(init "${SOURCES}" "${HEADERS}" "${INIT_LIBRARIES}")
add_dependencies(init localization)

set_target_properties(init PROPERTIES FOLDER linux)

# Generate initrd.img from the init binary
set(INIT_BINARY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/init")
set(INITRD_OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/initrd.img")

add_custom_command(
    OUTPUT ${INITRD_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E echo "Creating initrd.img..."
    COMMAND powershell -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tools/CreateInitrd.ps1" -InitPath "${INIT_BINARY}" -OutputPath "${INITRD_OUTPUT}"
    DEPENDS init ${INIT_BINARY}
    COMMENT "Generating initrd.img from init binary"
    VERBATIM
)

add_custom_target(initrd DEPENDS ${INITRD_OUTPUT})
add_dependencies(initrd init)
set_target_properties(initrd PROPERTIES FOLDER linux)
