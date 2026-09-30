set(NUGET_PACKAGES Microsoft.WSL.PluginApi.nuspec Microsoft.WSL.Containers.nuspec)
set(WSL_NUGET_TARGET_FRAMEWORK "net8.0-windows10.0.19041.0")

# Generate vars with native paths since nuget won't accept unix path separators.
cmake_path(NATIVE_PATH CMAKE_SOURCE_DIR CMAKE_SOURCE_DIR_NATIVE)
cmake_path(NATIVE_PATH CMAKE_BINARY_DIR CMAKE_BINARY_DIR_NATIVE)

# For the WSLC SDK, we allow packaging only one platform or not including the C# projection
# to make it easier to build a local package for development.
# The list files needed in each case are included conditionally from a nuspec fragment.

# Read a nuspec fragment .in file and expand its CMake variables into the named variable.
# Pass PLATFORM <arch> to bind the ${PLATFORM} placeholder in the fragment.
function(read_nuspec_fragment var path)
    cmake_parse_arguments(ARG "" "PLATFORM" "" ${ARGN})
    if(DEFINED ARG_PLATFORM)
        set(PLATFORM "${ARG_PLATFORM}")
    endif()
    file(READ "${path}" content)
    string(CONFIGURE "${content}" content)
    set(${var} "${content}" PARENT_SCOPE)
endfunction()

# C# SDK wrapper DLL — AnyCPU, produced for any TARGET_PLATFORM.
if(WSL_INCLUDE_SDK_CSHARP)
    read_nuspec_fragment(NUGET_SDKCS_FILE "${CMAKE_CURRENT_LIST_DIR}/fragments/wslc-cs.nuspec.in")
else()
    set(NUGET_SDKCS_FILE "")
endif()

# x64 native runtime — include when this is an x64 build, or for a multi-arch package.
if(TARGET_PLATFORM STREQUAL "x64" OR NOT WSL_NUGET_SINGLE_PLATFORM)
    read_nuspec_fragment(NUGET_X64_RUNTIMES "${CMAKE_CURRENT_LIST_DIR}/fragments/wslc-runtime.nuspec.in" PLATFORM x64)
else()
    set(NUGET_X64_RUNTIMES "")
endif()

# arm64 native runtime — include when this is an arm64 build, or for a multi-arch package.
if(TARGET_PLATFORM STREQUAL "arm64" OR NOT WSL_NUGET_SINGLE_PLATFORM)
    read_nuspec_fragment(NUGET_ARM64_RUNTIMES "${CMAKE_CURRENT_LIST_DIR}/fragments/wslc-runtime.nuspec.in" PLATFORM arm64)
else()
    set(NUGET_ARM64_RUNTIMES "")
endif()

# Generate the .nuspec files
foreach(PACKAGE ${NUGET_PACKAGES})
    configure_file("${CMAKE_CURRENT_LIST_DIR}/${PACKAGE}.in" "${CMAKE_BINARY_DIR}/${PACKAGE}")
endforeach()