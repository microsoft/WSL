if(TARGET Microsoft.WSL.Containers::sdk)
    return()
endif()

# Determine target architecture
if(CMAKE_GENERATOR_PLATFORM)
    string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" _wslcsdk_platform)
    if(_wslcsdk_platform STREQUAL "x64")
        set(_wslcsdk_arch "x64")
    elseif(_wslcsdk_platform STREQUAL "arm64")
        set(_wslcsdk_arch "arm64")
    else()
        message(FATAL_ERROR "Microsoft.WSL.Containers: Unsupported platform '${CMAKE_GENERATOR_PLATFORM}'. Supported: x64, ARM64.")
    endif()
    unset(_wslcsdk_platform)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Mm][Dd]64|[Xx]86_64|[Xx]64")
    set(_wslcsdk_arch "x64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]64|[Aa]arch64")
    set(_wslcsdk_arch "arm64")
else()
    message(FATAL_ERROR
        "Microsoft.WSL.Containers: Unsupported architecture '${CMAKE_SYSTEM_PROCESSOR}'. "
        "Supported: x64, ARM64.")
endif()

# Compute paths relative to package root (<root>/build/cmake/ -> <root>/)
get_filename_component(_wslcsdk_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_wslcsdk_include_dir "${_wslcsdk_root}/include")
set(_wslcsdk_lib_dir "${_wslcsdk_root}/runtimes/win-${_wslcsdk_arch}")

# Validate that expected files exist
if(NOT EXISTS "${_wslcsdk_include_dir}/wslcsdk.h")
    message(FATAL_ERROR "Microsoft.WSL.Containers: Cannot find wslcsdk.h in '${_wslcsdk_include_dir}'")
endif()

if(NOT EXISTS "${_wslcsdk_lib_dir}/wslcsdk.lib")
    message(FATAL_ERROR "Microsoft.WSL.Containers: Cannot find wslcsdk.lib in '${_wslcsdk_lib_dir}'")
endif()

# Create imported target
add_library(Microsoft.WSL.Containers::sdk SHARED IMPORTED GLOBAL)
set_target_properties(Microsoft.WSL.Containers::sdk PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_wslcsdk_include_dir}"
    IMPORTED_IMPLIB "${_wslcsdk_lib_dir}/wslcsdk.lib"
    IMPORTED_LOCATION "${_wslcsdk_lib_dir}/wslcsdk.dll"
)

# Standard find_package output variables
set(Microsoft.WSL.Containers_FOUND TRUE)
set(Microsoft.WSL.Containers_INCLUDE_DIRS "${_wslcsdk_include_dir}")
set(Microsoft.WSL.Containers_LIBRARIES Microsoft.WSL.Containers::sdk)

# Clean up temporary variables
unset(_wslcsdk_arch)
unset(_wslcsdk_root)
unset(_wslcsdk_include_dir)
unset(_wslcsdk_lib_dir)
