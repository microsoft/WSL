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

function(wslc_add_image)
    cmake_parse_arguments(
        PARSE_ARGV 0 ARG
        ""                                      # options (none)
        "NAME;TAG;DOCKERFILE;CONTEXT;OUTPUT"     # one-value keywords
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
    if(NOT ARG_TAG)
        set(ARG_TAG "latest")
    endif()
    if(NOT ARG_OUTPUT)
        set(ARG_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}")
    endif()

    set(_image_ref "${ARG_NAME}:${ARG_TAG}")
    set(_tar_output "${ARG_OUTPUT}/${ARG_NAME}.tar")
    set(_id_output "${ARG_OUTPUT}/${ARG_NAME}.id")
    set(_marker "${CMAKE_CURRENT_BINARY_DIR}/wslc_${ARG_NAME}.marker")

    # Resolve source globs to file lists
    file(GLOB_RECURSE _resolved_sources ${ARG_SOURCES})

    add_custom_command(
        OUTPUT "${_marker}"
        COMMAND wslc image build -t "${_image_ref}" -f "${ARG_DOCKERFILE}" "${ARG_CONTEXT}"
        COMMAND wslc image save "${_image_ref}" -o "${_tar_output}"
        COMMAND ${CMAKE_COMMAND} -E touch "${_marker}"
        DEPENDS ${_resolved_sources} "${ARG_DOCKERFILE}"
        COMMENT "WSLC: Building image '${_image_ref}'..."
        VERBATIM
    )

    add_custom_target(wslc_image_${ARG_NAME} ALL
        DEPENDS "${_marker}"
    )
endfunction()
