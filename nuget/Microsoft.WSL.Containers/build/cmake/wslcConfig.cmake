# wslcConfig.cmake - Container image build support for CMake
#
# Provides the wslc_add_image() function for declaring container image
# build targets with incremental rebuild support.
#
# Usage:
#   list(APPEND CMAKE_MODULE_PATH "<nuget>/build/cmake")
#   find_package(wslc REQUIRED)
#
#   wslc_add_image(
#       NAME        my-server
#       DOCKERFILE  container/Dockerfile
#       CONTEXT     container/
#       SOURCES     container/src/*.cpp container/src/*.h
#       TAG         latest
#       OUTPUT      ${CMAKE_BINARY_DIR}/images
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
        "NAME;IMAGE;TAG;DOCKERFILE;CONTEXT;OUTPUT" # one-value keywords
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
    if(NOT ARG_OUTPUT)
        set(ARG_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}")
    endif()

    # Find wslc CLI
    if(NOT WSLC_CLI_PATH)
        find_program(WSLC_CLI_PATH wslc PATHS "$ENV{ProgramW6432}/WSL" "$ENV{ProgramFiles}/WSL")
        if(NOT WSLC_CLI_PATH)
            message(FATAL_ERROR "wslc CLI not found. Install WSL by running: wsl --install --no-distribution")
        endif()
    endif()

    set(_image_ref "${ARG_IMAGE}:${ARG_TAG}")
    set(_marker "${CMAKE_CURRENT_BINARY_DIR}/wslc_${ARG_NAME}.marker")
    # TODO: set(_tar_output "${ARG_OUTPUT}/${ARG_NAME}.tar") when wslc image save is available

    # Resolve source globs to file lists
    file(GLOB_RECURSE _resolved_sources CONFIGURE_DEPENDS ${ARG_SOURCES})

    add_custom_command(
        OUTPUT "${_marker}"
        COMMAND "${WSLC_CLI_PATH}" image build -t "${_image_ref}" -f "${ARG_DOCKERFILE}" "${ARG_CONTEXT}"
        COMMAND ${CMAKE_COMMAND} -E touch "${_marker}"
        DEPENDS ${_resolved_sources} "${ARG_DOCKERFILE}"
        COMMENT "WSLC: Building image '${_image_ref}'..."
        VERBATIM
    )

    add_custom_target(wslc_image_${ARG_NAME} ALL
        DEPENDS "${_marker}"
    )
endfunction()
