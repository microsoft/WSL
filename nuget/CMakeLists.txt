set(NUGET_PACKAGES Microsoft.WSL.PluginApi.nuspec Microsoft.WSL.Containers.nuspec)
set(WSL_NUGET_TARGET_FRAMEWORK "net8.0-windows10.0.19041.0")

# generate vars with native paths since nuget won't accept unix path separators
cmake_path(NATIVE_PATH CMAKE_SOURCE_DIR CMAKE_SOURCE_DIR_NATIVE)
cmake_path(NATIVE_PATH CMAKE_BINARY_DIR CMAKE_BINARY_DIR_NATIVE)

# When ON, the nuspec only includes binaries for the currently configured platform.
# Enables local nuget builds without requiring both x64 and arm64 to be built.
option(WSL_NUGET_SINGLE_ARCH "Include only the current platform's binaries in the NuGet package (for local testing)" OFF)

string(TOLOWER "${TARGET_PLATFORM}" _wslcsdk_platform_lower)

if(WSL_NUGET_SINGLE_ARCH)
    # Single-arch: only include the current platform's native binaries.
    set(WSL_NUGET_ARCH_ENTRIES "    <file src=\"${CMAKE_SOURCE_DIR_NATIVE}\\bin\\${TARGET_PLATFORM}\\${CMAKE_BUILD_TYPE}\\wslcsdk.lib\" target=\"runtimes\\win-${_wslcsdk_platform_lower}\\\"/>
    <file src=\"${CMAKE_SOURCE_DIR_NATIVE}\\bin\\${TARGET_PLATFORM}\\${CMAKE_BUILD_TYPE}\\wslcsdk.dll\" target=\"runtimes\\win-${_wslcsdk_platform_lower}\\native\\\"/>")
else()
    # Dual-arch (default): include both x64 and arm64 native binaries, as the pipeline does.
    set(WSL_NUGET_ARCH_ENTRIES "    <file src=\"${CMAKE_SOURCE_DIR_NATIVE}\\bin\\x64\\${CMAKE_BUILD_TYPE}\\wslcsdk.lib\" target=\"runtimes\\win-x64\\\"/>
    <file src=\"${CMAKE_SOURCE_DIR_NATIVE}\\bin\\x64\\${CMAKE_BUILD_TYPE}\\wslcsdk.dll\" target=\"runtimes\\win-x64\\native\\\"/>

    <file src=\"${CMAKE_SOURCE_DIR_NATIVE}\\bin\\arm64\\${CMAKE_BUILD_TYPE}\\wslcsdk.lib\" target=\"runtimes\\win-arm64\\\"/>
    <file src=\"${CMAKE_SOURCE_DIR_NATIVE}\\bin\\arm64\\${CMAKE_BUILD_TYPE}\\wslcsdk.dll\" target=\"runtimes\\win-arm64\\native\\\"/>")
endif()

# wslcsdkcs.dll (the C# projection) is only built when WSL_BUILD_SDKCS=ON.
if(WSL_BUILD_SDKCS)
    set(WSL_NUGET_CSWINRT_ENTRY "    <file src=\"${CMAKE_SOURCE_DIR_NATIVE}\\bin\\x64\\${CMAKE_BUILD_TYPE}\\wslcsdkcs.dll\" target=\"lib\\${WSL_NUGET_TARGET_FRAMEWORK}\\\"/>")
else()
    set(WSL_NUGET_CSWINRT_ENTRY "")
endif()

unset(_wslcsdk_platform_lower)

foreach(PACKAGE ${NUGET_PACKAGES})
    configure_file("${CMAKE_CURRENT_LIST_DIR}/${PACKAGE}.in" "${CMAKE_BINARY_DIR}/${PACKAGE}")
endforeach()

# Target to build the Microsoft.WSL.Containers NuGet package locally.
# Usage: cmake --build . --target nuget_containers
# The package is placed in ${CMAKE_BINARY_DIR}/out/nuget/.
set(_nuget_out "${CMAKE_BINARY_DIR}/out/nuget")
add_custom_target(nuget_containers
    COMMAND "${CMAKE_BINARY_DIR}/_deps/nuget.exe" pack "${CMAKE_BINARY_DIR}/Microsoft.WSL.Containers.nuspec"
            -OutputDirectory "${_nuget_out}" -NonInteractive
    COMMENT "Packing Microsoft.WSL.Containers -> ${_nuget_out}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
unset(_nuget_out)