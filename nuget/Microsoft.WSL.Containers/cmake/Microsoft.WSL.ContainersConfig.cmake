if(TARGET Microsoft.WSL.Containers::SDK)
    return()
endif()

if(NOT WIN32)
    message(FATAL_ERROR "Microsoft.WSL.Containers: This package only supports Windows.")
endif()

# Determine target architecture
if(CMAKE_GENERATOR_PLATFORM)
    string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" _wslcsdk_platform)
    if(_wslcsdk_platform STREQUAL "x64")
        set(_wslcsdk_arch "x64")
    elseif(_wslcsdk_platform STREQUAL "arm64")
        set(_wslcsdk_arch "arm64")
    else()
        message(FATAL_ERROR
            "Microsoft.WSL.Containers: Unsupported platform '${CMAKE_GENERATOR_PLATFORM}'."
            " Supported: x64, ARM64.")
    endif()
    unset(_wslcsdk_platform)
elseif(CMAKE_SYSTEM_PROCESSOR)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _wslcsdk_platform)
    if(_wslcsdk_platform MATCHES "amd64|x86_64|x64")
        set(_wslcsdk_arch "x64")
    elseif(_wslcsdk_platform MATCHES "arm64|aarch64")
        set(_wslcsdk_arch "arm64")
    else()
        message(FATAL_ERROR
            "Microsoft.WSL.Containers: Unsupported architecture '${CMAKE_SYSTEM_PROCESSOR}'."
            " Supported: x64, ARM64.")
    endif()
    unset(_wslcsdk_platform)
else()
    message(FATAL_ERROR
        "Microsoft.WSL.Containers: Could not determine target architecture."
        " Set CMAKE_GENERATOR_PLATFORM or CMAKE_SYSTEM_PROCESSOR.")
endif()

# Compute paths relative to package root (<root>/cmake/ -> <root>/)
get_filename_component(_wslcsdk_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_wslcsdk_include_dir "${_wslcsdk_root}/include")
set(_wslcsdk_lib_dir "${_wslcsdk_root}/runtimes/win-${_wslcsdk_arch}")

# Create imported target
add_library(Microsoft.WSL.Containers::SDK SHARED IMPORTED GLOBAL)
set_target_properties(Microsoft.WSL.Containers::SDK PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_wslcsdk_include_dir}"
    IMPORTED_IMPLIB "${_wslcsdk_lib_dir}/wslcsdk.lib"
    IMPORTED_LOCATION "${_wslcsdk_lib_dir}/wslcsdk.dll"
)

# Clean up temporary variables
unset(_wslcsdk_arch)
unset(_wslcsdk_root)
unset(_wslcsdk_include_dir)
unset(_wslcsdk_lib_dir)
