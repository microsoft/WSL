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
    IMPORTED_LOCATION "${_wslcsdk_lib_dir}/native/wslcsdk.dll"
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
#       IMAGE        ghcr.io/myorg/my-server:latest
#       DOCKERFILE   container/Dockerfile
#       CONTEXT      container/
#       SOURCES      container/src/*.cpp container/src/*.h
#       TAR_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/my-server.tar
#   )
#
#   add_dependencies(my_app my-server)
#
# The first positional argument is the CMake target name.
# IMAGE is the container image reference (required); may include a tag
# (e.g. 'my-server:v1'). ':latest' is appended automatically when omitted.
# TAR_LOCATION is the output path for the saved image tarball
# (optional; defaults to ${CMAKE_CURRENT_BINARY_DIR}/<target>.tar).
# Pass PRUNE_AFTER_BUILD to also run 'wslc image prune' after save.

function(wslc_add_image _target_name)
    cmake_parse_arguments(
        PARSE_ARGV 1 ARG
        "PRUNE_AFTER_BUILD"                          # options (boolean flags)
        "IMAGE;DOCKERFILE;CONTEXT;TAR_LOCATION"      # one-value keywords
        "SOURCES"                                    # multi-value keywords
    )

    # Reject typos / unknown keywords so they can't silently slip through.
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "wslc_add_image: unknown argument(s): ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    # Validate required arguments
    if(NOT ARG_IMAGE)
        message(FATAL_ERROR "wslc_add_image: IMAGE is required")
    endif()
    if(NOT ARG_DOCKERFILE)
        message(FATAL_ERROR "wslc_add_image: DOCKERFILE is required")
    endif()
    if(NOT ARG_CONTEXT)
        message(FATAL_ERROR "wslc_add_image: CONTEXT is required")
    endif()

    # Append :latest when IMAGE has no tag. Detect by looking for ':' after the
    # last '/', so registry-port refs like localhost:5000/repo aren't misread.
    string(FIND "${ARG_IMAGE}" "/" _last_slash_pos REVERSE)
    string(FIND "${ARG_IMAGE}" ":" _last_colon_pos REVERSE)
    if(_last_colon_pos GREATER _last_slash_pos)
        set(_image_ref "${ARG_IMAGE}")
    else()
        set(_image_ref "${ARG_IMAGE}:latest")
    endif()

    # Defaults
    if(NOT ARG_TAR_LOCATION)
        set(ARG_TAR_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/${_target_name}.tar")
    endif()
    # Normalize TAR_LOCATION to an absolute path. A bare filename or relative
    # path would leave _tar_dir empty below and break `make_directory ""`.
    # Skip the normalization when the path contains a generator expression
    # (e.g. $<TARGET_FILE_DIR:...>) — those resolve to absolute paths at
    # build time and would otherwise get BASE_DIR prepended at configure
    # time, producing a doubled path like build/$<...>/foo.tar.
    if(NOT ARG_TAR_LOCATION MATCHES "\\$<")
        get_filename_component(ARG_TAR_LOCATION "${ARG_TAR_LOCATION}" ABSOLUTE
                               BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    endif()

    # Find wslc CLI on PATH (the WSL MSI puts it there).
    if(NOT WSLC_CLI_PATH)
        find_program(WSLC_CLI_PATH wslc)
        if(NOT WSLC_CLI_PATH)
            message(FATAL_ERROR "wslc CLI not found on PATH. Install WSL by running 'wsl --install --no-distribution', or set the WSLC_CLI_PATH variable to a specific wslc.exe path.")
        endif()
    endif()

    # Validate target name (used as CMake target id and default tar filename).
    string(REGEX MATCH "[^a-zA-Z0-9_.+-]" _bad_char "${_target_name}")
    if(_bad_char)
        message(FATAL_ERROR "wslc_add_image: '${_target_name}' contains unsupported character '${_bad_char}'. The target name is used as a CMake target identifier and as the default tar filename, so it must be limited to letters, digits, '_', '.', '+', and '-'.")
    endif()

    # Normalize paths to be independent of the build directory
    get_filename_component(_dockerfile_path "${ARG_DOCKERFILE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(_context_path "${ARG_CONTEXT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

    # Resolve source globs to file lists; default to CONTEXT contents if SOURCES omitted
    if(ARG_SOURCES)
        file(GLOB_RECURSE _resolved_sources CONFIGURE_DEPENDS ${ARG_SOURCES})
    else()
        file(GLOB_RECURSE _resolved_sources CONFIGURE_DEPENDS "${_context_path}/*")
    endif()

    get_filename_component(_tar_dir "${ARG_TAR_LOCATION}" DIRECTORY)

    # Prune failure is swallowed by default (housekeeping); set
    # WSLC_TREAT_PRUNE_FAILURE_AS_ERROR=ON to fail the build on prune failure.
    set(_prune_command "")
    set(_prune_comment "")
    if(ARG_PRUNE_AFTER_BUILD)
        if(WSLC_TREAT_PRUNE_FAILURE_AS_ERROR)
            set(_prune_command COMMAND "${WSLC_CLI_PATH}" image prune)
        else()
            set(_prune_wrapper "${CMAKE_CURRENT_BINARY_DIR}/wslc_prune_ignore_failure.cmake")
            if(NOT EXISTS "${_prune_wrapper}")
                file(WRITE "${_prune_wrapper}"
                    "execute_process(COMMAND \"\${WSLC}\" image prune)\n")
            endif()
            set(_prune_command COMMAND "${CMAKE_COMMAND}" "-DWSLC=${WSLC_CLI_PATH}" -P "${_prune_wrapper}")
        endif()
        set(_prune_comment ", and pruning dangling images")
    endif()

    # Save to a .tmp and atomically rename on success — wslc image save uses
    # CREATE_ALWAYS, which truncates the destination on entry, so a failed
    # save would otherwise leave a partial tar with newer mtime than sources
    # (and break incremental). The rename only happens if save succeeded.
    add_custom_command(
        OUTPUT "${ARG_TAR_LOCATION}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_tar_dir}"
        COMMAND "${WSLC_CLI_PATH}" image build -t "${_image_ref}" -f "${_dockerfile_path}" "${_context_path}"
        COMMAND "${WSLC_CLI_PATH}" image save -o "${ARG_TAR_LOCATION}.tmp" "${_image_ref}"
        COMMAND ${CMAKE_COMMAND} -E rename "${ARG_TAR_LOCATION}.tmp" "${ARG_TAR_LOCATION}"
        ${_prune_command}
        DEPENDS ${_resolved_sources} "${_dockerfile_path}"
        COMMENT "WSLC: Building image '${_image_ref}', saving to '${ARG_TAR_LOCATION}'${_prune_comment}..."
        VERBATIM
    )

    add_custom_target(${_target_name} DEPENDS "${ARG_TAR_LOCATION}")
endfunction()
