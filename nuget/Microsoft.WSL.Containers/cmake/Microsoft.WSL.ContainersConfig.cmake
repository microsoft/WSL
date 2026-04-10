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
#   wslc_add_image(my-server
#       IMAGE       ghcr.io/myorg/my-server
#       DOCKERFILE  container/Dockerfile
#       CONTEXT     container/
#       SOURCES     container/src/*.cpp container/src/*.h
#       TAG         latest
#   )
#
#   add_dependencies(my_app my-server)
#
# The first positional argument is the CMake target name.
# IMAGE is the container image reference (required).

function(wslc_add_image _target_name)
    cmake_parse_arguments(
        PARSE_ARGV 1 ARG
        ""                                      # options (none)
        "IMAGE;TAG;DOCKERFILE;CONTEXT"           # one-value keywords
        "SOURCES"                                # multi-value keywords
    )

    # Validate required arguments
    if(NOT ARG_IMAGE)
        message(FATAL_ERROR "wslc_add_image: IMAGE is required")
    endif()
    # IMAGE must not contain a tag — use the TAG parameter instead
    string(FIND "${ARG_IMAGE}" ":" _colon_pos)
    if(NOT _colon_pos EQUAL -1)
        message(FATAL_ERROR "wslc_add_image: IMAGE '${ARG_IMAGE}' contains ':'. Specify the tag separately with the TAG parameter.")
    endif()
    if(NOT ARG_DOCKERFILE)
        message(FATAL_ERROR "wslc_add_image: DOCKERFILE is required")
    endif()
    if(NOT ARG_CONTEXT)
        message(FATAL_ERROR "wslc_add_image: CONTEXT is required")
    endif()

    # Defaults
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

    # Validate target name
    string(REGEX MATCH "[^a-zA-Z0-9_.+-]" _bad_char "${_target_name}")
    if(_bad_char)
        message(FATAL_ERROR "wslc_add_image: '${_target_name}' contains unsupported character '${_bad_char}'. Supported characters are letters, digits, '_', '.', '+', and '-'.")
    endif()

    # Normalize paths to be independent of the build directory
    get_filename_component(_dockerfile_path "${ARG_DOCKERFILE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(_context_path "${ARG_CONTEXT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

    set(_image_ref "${ARG_IMAGE}:${ARG_TAG}")
    set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/wslc_${_target_name}.built")

    # Resolve source globs to file lists; default to CONTEXT contents if SOURCES omitted
    if(ARG_SOURCES)
        file(GLOB_RECURSE _resolved_sources CONFIGURE_DEPENDS ${ARG_SOURCES})
    else()
        file(GLOB_RECURSE _resolved_sources CONFIGURE_DEPENDS "${_context_path}/*")
    endif()

    add_custom_command(
        OUTPUT "${_stamp}"
        COMMAND "${WSLC_CLI_PATH}" image build -t "${_image_ref}" -f "${_dockerfile_path}" "${_context_path}"
        COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
        DEPENDS ${_resolved_sources} "${_dockerfile_path}"
        COMMENT "WSLC: Building image '${_image_ref}'..."
        VERBATIM
    )

    add_custom_target(${_target_name} ALL
        DEPENDS "${_stamp}"
    )
endfunction()
