add_idl_winrt(wslcsdkwinrtidl "wslcsdk.idl")
set_target_properties(wslcsdkwinrtidl PROPERTIES FOLDER windows)

set(SOURCES
    Container.cpp
    ContainerNamedVolume.cpp
    ContainerPortMapping.cpp
    ContainerSettings.cpp
    ContainerVolume.cpp
    ImageInfo.cpp
    ImageProgress.cpp
    InstallProgress.cpp
    Process.cpp
    ProcessCrashInformation.cpp
    ProcessSettings.cpp
    PullImageOptions.cpp
    PushImageOptions.cpp
    ServiceVersion.cpp
    Session.cpp
    SessionSettings.cpp
    Streams.cpp
    TagImageOptions.cpp
    VhdOptions.cpp
    WslcService.cpp
)

set(HEADERS
    Container.h
    ContainerNamedVolume.h
    ContainerPortMapping.h
    ContainerSettings.h
    ContainerVolume.h
    Helpers.h
    ImageInfo.h
    ImageProgress.h
    InstallProgress.h
    Process.h
    ProcessCrashInformation.h
    ProcessSettings.h
    PullImageOptions.h
    PushImageOptions.h
    ServiceVersion.h
    Session.h
    SessionSettings.h
    Streams.h
    TagImageOptions.h
    VhdOptions.h
    WslcService.h
)

add_library(wslcsdkwinrt STATIC ${SOURCES} ${HEADERS})
add_dependencies(wslcsdkwinrt wslcsdkwinrtidl)

set(MODULE_G_CPP ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/module.g.cpp)
target_sources(wslcsdkwinrt PRIVATE ${MODULE_G_CPP})
set_source_files_properties(
    ${MODULE_G_CPP}
    PROPERTIES
    GENERATED TRUE)

target_precompile_headers(wslcsdkwinrt PRIVATE precomp.h)
target_include_directories(wslcsdkwinrt PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}
    ${CMAKE_SOURCE_DIR}/src/windows/WslcSDK
)

set_target_properties(
    wslcsdkwinrt
    PROPERTIES
    FOLDER windows
    EXCLUDE_FROM_ALL FALSE
)

# Copy the winmd to the bin output directory so the nuspec can reference it
# from a stable, arch-aware path (bin\<platform>\<config>\Microsoft.WSL.Containers.winmd).
set(WINMD_SRC ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/wslcsdk.winmd)
set(WINMD_DEST ${CMAKE_SOURCE_DIR}/bin/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/Microsoft.WSL.Containers.winmd)

add_custom_command(TARGET wslcsdkwinrtidl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/bin/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WINMD_SRC}" "${WINMD_DEST}"
    COMMENT "Copying Microsoft.WSL.Containers.winmd to bin output"
)
