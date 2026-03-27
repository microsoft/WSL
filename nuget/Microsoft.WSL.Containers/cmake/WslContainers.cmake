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
    cmake_parse_arguments(PARSE_ARGV 0 WSLC "" "NAME;DOCKERFILE;CONTEXT;TAG" "DEPENDS")

    if(NOT WSLC_NAME)
        message(FATAL_ERROR "wslc_add_image: NAME is required")
    endif()

    if(NOT WSLC_DOCKERFILE)
        message(FATAL_ERROR "wslc_add_image: DOCKERFILE is required")
    endif()

    if(NOT WSLC_EXECUTABLE)
        message(FATAL_ERROR "wslc_add_image: wslc executable not found. Install wslc or set WSLC_EXECUTABLE.")
    endif()

    if(NOT WSLC_CONTEXT)
        cmake_path(GET WSLC_DOCKERFILE PARENT_PATH WSLC_CONTEXT)
    endif()

    if(NOT WSLC_TAG)
        set(WSLC_TAG "latest")
    endif()

    set(TARGET_NAME "wslc_image_${WSLC_NAME}")

    add_custom_target(${TARGET_NAME}
        COMMAND ${WSLC_EXECUTABLE} build ${WSLC_CONTEXT} -f ${WSLC_DOCKERFILE} -t ${WSLC_NAME}:${WSLC_TAG}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Building WSLC container image: ${WSLC_NAME}"
        VERBATIM
    )

    # Wire up dependencies: ensure DEPENDS targets build before the container image
    if(WSLC_DEPENDS)
        add_dependencies(${TARGET_NAME} ${WSLC_DEPENDS})
    endif()
endfunction()
