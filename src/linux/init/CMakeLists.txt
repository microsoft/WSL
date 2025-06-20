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

set(INITRAMFS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/initrd.img)
set(INIT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/init)

# The initramfs depends on both the init target and binary so it doesn't start too early
# (before the symbols are stripped from the init binary)
# but so that it is still rebuilt if the file itself changes.
add_custom_command(
    OUTPUT ${INITRAMFS} "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/initramfs"
    DEPENDS init ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/init
    COMMAND ${PERL_SOURCE_DIR}/bin/perl.exe ${BIN2CPIO} ${INIT} ${INITRAMFS}
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/initramfs"
    VERBATIM
    )

add_custom_target(initramfs DEPENDS ${INITRAMFS})

set_target_properties(init PROPERTIES FOLDER linux)
set_target_properties(initramfs PROPERTIES FOLDER linux)
