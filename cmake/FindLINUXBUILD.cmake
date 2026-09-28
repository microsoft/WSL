function(build_linux_objects sources headers)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE})

    foreach(e ${sources})
        cmake_path(GET e FILENAME object_name)
        set(object "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/${object_name}.o")
        set(objects ${objects} ${object})

        if("${e}" MATCHES "^.*\.c$")
            set(compiler ${LINUX_CC})
            set(flags ${LINUX_CFLAGS})
        else()
            set(compiler ${LINUX_CXX})
            set(flags ${LINUX_CXXFLAGS})
        endif()

        add_custom_command(
            OUTPUT ${object}
            COMMAND ${compiler} ${flags} ${LINUX_BUILD_TYPE_FLAGS} ${e} -c -o ${object}
            WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
            MAIN_DEPENDENCY ${e}
            DEPENDS ${headers} # Every object depends on all headers to trigger a rebuild if a header is changed
            VERBATIM
        )
    endforeach()

    set(objects ${objects} PARENT_SCOPE)
endfunction()

function(add_linux_library_impl target sources headers add_sources)
    set(ar_output "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/${target}.a")

    build_linux_objects("${sources}" "${headers}")

    add_custom_command(
        OUTPUT ${ar_output} ${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/${target}
        COMMAND ${LINUX_AR} crus ${ar_output} ${objects}
        COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/${target}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS ${objects}
        VERBATIM
    )

    if (${add_sources})
        add_custom_target(${target} DEPENDS ${ar_output} SOURCES ${sources} ${headers})
    else()
        add_custom_target(${target} DEPENDS ${ar_output})
    endif()

    set_source_files_properties(${ar_output} PROPERTIES GENERATED TRUE)
endfunction()

function(add_linux_library target sources headers)
    add_linux_library_impl(${target} "${sources}" "${headers}" TRUE)
endfunction()

function(add_linux_library_no_sources target sources headers)
    add_linux_library_impl(${target} "${sources}" "${headers}" FALSE)
endfunction()


function(add_linux_executable target sources headers libraries)
    set(output "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/${target}")
    set(output_unstripped "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/${target}.unstripped")

    build_linux_objects("${sources}" "${headers}")

    set(libs -lunwind -lc++abi -lc++)
    foreach(e ${libraries})
        set(libs ${libs} -l${e})

        # Note: This makes the assumption that all libraries are static (.a and not .so).
        # Executables need to depend on both the target and the underlying library file, so that
        # the libraries target get analyzed for changes, and the executable gets linked again if the .a files changed.
        list(APPEND lib_targets "lib${e}")
        list(APPEND lib_files "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/lib${e}.a")
    endforeach()

    if (NOT ${CMAKE_BUILD_TYPE} STREQUAL "Debug")
        set(stripped_output "${output}")
        set(output "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/${target}.debug")
        add_custom_command(
            OUTPUT ${stripped_output}
            COMMAND ${LLVM_INSTALL_DIR}/llvm-strip.exe "${output}" -o "${stripped_output}"
            COMMAND ${LLVM_INSTALL_DIR}/llvm-objcopy.exe --add-gnu-debuglink "${output}" "${stripped_output}"
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DEPENDS ${output}
            VERBATIM
        )
     endif()

    add_custom_command(
         OUTPUT ${output}
         COMMAND ${LINUX_CXX} -o ${output} ${LINUXSDK_PATH}/lib/crti.o ${LINUXSDK_PATH}/lib/crt1.o ${objects} ${LINUXSDK_PATH}/lib/crtn.o ${LINUX_LDFLAGS} ${libs}
         COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/${target}"
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
         DEPENDS ${objects} ${lib_files}
         VERBATIM
     )

    add_custom_target(${target} DEPENDS ${output} ${stripped_output} SOURCES ${sources} ${headers})
    add_dependencies(${target} ${lib_targets})

endfunction()