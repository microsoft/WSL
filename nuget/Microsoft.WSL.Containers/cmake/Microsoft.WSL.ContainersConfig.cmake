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

# ============================================================================
# Container Image Build Targets
# ============================================================================
#
# Provides the wslc_add_image() function for declaring container image
# build targets with incremental rebuild support.
#
# Usage:
#   find_package(Microsoft.WSL.Containers REQUIRED)
#
#   wslc_add_image(
#       NAME        my-server
#       DOCKERFILE  container/Dockerfile
#       CONTEXT     container/
#       SOURCES     container/src/*.cpp container/src/*.h
#       TAG         latest
#   )
#
#   # With explicit image registry/name (IMAGE defaults to NAME if omitted):
#   wslc_add_image(
#       NAME        my-server
#       IMAGE       ghcr.io/myorg/my-server
#       TAG         v1.2.3
#       DOCKERFILE  container/Dockerfile
#       CONTEXT     container/
#   )

function(wslc_add_image)
    cmake_parse_arguments(
        PARSE_ARGV 0 ARG
        ""                                      # options (none)
        "NAME;IMAGE;TAG;DOCKERFILE;CONTEXT"        # one-value keywords
        "SOURCES"                                # multi-value keywords
    )

    # Validate required arguments
    if(NOT ARG_NAME)
        message(FATAL_ERROR "wslc_add_image: NAME is required")
    endif()
    if(NOT ARG_DOCKERFILE)
        message(FATAL_ERROR "wslc_add_image: DOCKERFILE is required")
    endif()
    if(NOT ARG_CONTEXT)
        message(FATAL_ERROR "wslc_add_image: CONTEXT is required")
    endif()

    # Defaults
    if(NOT ARG_IMAGE)
        set(ARG_IMAGE "${ARG_NAME}")
    endif()
    if(NOT ARG_TAG)
        set(ARG_TAG "latest")
    endif()

    # Find wslc CLI
    if(NOT WSLC_CLI_PATH)
        find_program(WSLC_CLI_PATH wslc PATHS "$ENV{ProgramW6432}/WSL" "$ENV{ProgramFiles}/WSL")
        if(NOT WSLC_CLI_PATH)
            message(FATAL_ERROR "wslc CLI not found. Install WSL by running: wsl --install --no-distribution")
        endif()
    endif()

    # Validate NAME is usable as a CMake target name
    string(REGEX MATCH "[^a-zA-Z0-9_.-]" _bad_char "${ARG_NAME}")
    if(_bad_char)
        message(FATAL_ERROR "wslc_add_image: NAME '${ARG_NAME}' contains invalid character '${_bad_char}'. Use IMAGE for the full registry/name reference.")
    endif()

    # Normalize paths to be independent of the build directory
    get_filename_component(_dockerfile_path "${ARG_DOCKERFILE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(_context_path "${ARG_CONTEXT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

    set(_image_ref "${ARG_IMAGE}:${ARG_TAG}")
    set(_marker "${CMAKE_CURRENT_BINARY_DIR}/wslc_${ARG_NAME}.marker")

    # Resolve source globs to file lists; default to CONTEXT contents if SOURCES omitted
    if(ARG_SOURCES)
        file(GLOB_RECURSE _resolved_sources CONFIGURE_DEPENDS ${ARG_SOURCES})
    else()
        file(GLOB_RECURSE _resolved_sources CONFIGURE_DEPENDS "${_context_path}/*")
    endif()

    add_custom_command(
        OUTPUT "${_marker}"
        COMMAND "${WSLC_CLI_PATH}" image build -t "${_image_ref}" -f "${_dockerfile_path}" "${_context_path}"
        COMMAND ${CMAKE_COMMAND} -E touch "${_marker}"
        DEPENDS ${_resolved_sources} "${_dockerfile_path}"
        COMMENT "WSLC: Building image '${_image_ref}'..."
        VERBATIM
    )

    add_custom_target(wslc_image_${ARG_NAME} ALL
        DEPENDS "${_marker}"
    )
endfunction()
