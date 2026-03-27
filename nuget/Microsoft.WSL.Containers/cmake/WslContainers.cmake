#[[

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslContainers.cmake

Abstract:

    CMake module to build WSLC container images using the wslc CLI.

Usage:
    include(WslContainers)
    wslc_add_image(NAME my-server DOCKERFILE container/Dockerfile)
    add_dependencies(my_app wslc_image_my-server)

]]

find_program(WSLC_EXECUTABLE wslc)

function(wslc_add_image)
    cmake_parse_arguments(PARSE_ARGV 0 _WSLC "" "NAME;DOCKERFILE;CONTEXT;TAG" "DEPENDS")

    if(NOT _WSLC_NAME)
        message(FATAL_ERROR "wslc_add_image: NAME is required")
    endif()

    if(NOT _WSLC_DOCKERFILE)
        message(FATAL_ERROR "wslc_add_image: DOCKERFILE is required")
    endif()

    if(NOT WSLC_EXECUTABLE)
        message(WARNING "wslc_add_image: wslc not found on PATH. "
                "Container image targets will be created but may fail at build time. "
                "Set WSLC_EXECUTABLE to the wslc path to resolve.")
        set(WSLC_EXECUTABLE "wslc")
    endif()

    if(NOT _WSLC_CONTEXT)
        cmake_path(GET _WSLC_DOCKERFILE PARENT_PATH _WSLC_CONTEXT)
    endif()

    if(NOT _WSLC_TAG)
        set(_WSLC_TAG "latest")
    endif()

    set(TARGET_NAME "wslc_image_${_WSLC_NAME}")

    add_custom_target(${TARGET_NAME}
        COMMAND ${WSLC_EXECUTABLE} build ${_WSLC_CONTEXT} -f ${_WSLC_DOCKERFILE} -t ${_WSLC_NAME}:${_WSLC_TAG}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Building WSLC container image: ${_WSLC_NAME}"
        VERBATIM
    )

    # Wire up dependencies: ensure DEPENDS targets build before the container image
    if(_WSLC_DEPENDS)
        add_dependencies(${TARGET_NAME} ${_WSLC_DEPENDS})
    endif()
endfunction()
