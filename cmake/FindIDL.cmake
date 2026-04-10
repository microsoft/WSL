function(add_idl target idl_files_with_proxy idl_files_no_proxy)
    set(OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE})
    file(MAKE_DIRECTORY ${OUTPUT_DIR})
    set(TARGET_OUTPUTS)

    set(IDL_DEFINITIONS "")

    get_directory_property(IDL_DEFS COMPILE_DEFINITIONS )
    foreach(e ${IDL_DEFS})
        set(IDL_DEFINITIONS ${IDL_DEFINITIONS} /D${e})
    endforeach()

    string(TOLOWER ${TARGET_PLATFORM} IDL_ENV)
    set(PREVIOUS_OUTPUT "")

    set(IDL_DLLDATA ${OUTPUT_DIR}/dlldata_${TARGET_PLATFORM}.c)

    list(LENGTH idl_files_with_proxy PROXY_IDL_COUNT)
    set(PROXY_IDL_INDEX 0)

    foreach(idl_file ${idl_files_with_proxy})

        cmake_path(GET idl_file STEM IDL_NAME)

        set(IDL_HEADER ${OUTPUT_DIR}/${IDL_NAME}.h)

        # Adding a _${TARGET_PLATFORM} to work around object files having
        # the same paths regardless of TARGET_PLATFORM, which can cause the linker to fail with:
        # "fatal error LNK1112: module machine type 'x64' conflicts with target machine type 'ARM64'"
        set(IDL_I ${OUTPUT_DIR}/${IDL_NAME}_i_${TARGET_PLATFORM}.c)
        set(IDL_P ${OUTPUT_DIR}/${IDL_NAME}_p_${TARGET_PLATFORM}.c)

        # Only list dlldata as a tracked output of the last MIDL command.
        math(EXPR PROXY_IDL_INDEX "${PROXY_IDL_INDEX} + 1")
        if(PROXY_IDL_INDEX EQUAL PROXY_IDL_COUNT)
            set(MIDL_OUTPUT ${IDL_HEADER} ${IDL_I} ${IDL_P} ${IDL_DLLDATA})
        else()
            set(MIDL_OUTPUT ${IDL_HEADER} ${IDL_I} ${IDL_P})
        endif()

        add_custom_command(
            OUTPUT ${MIDL_OUTPUT}
            COMMAND midl /nologo /target NT100 /env "${IDL_ENV}" /Zp8 /char unsigned /ms_ext /c_ext /h ${IDL_HEADER} /iid ${IDL_I} /proxy ${IDL_P} /dlldata ${IDL_DLLDATA} ${idl_file} ${IDL_DEFINITIONS}
            WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
            DEPENDS ${idl_file} ${PREVIOUS_OUTPUT}
            MAIN_DEPENDENCY ${idl_file}
            VERBATIM
        )

        set(PREVIOUS_OUTPUT ${IDL_HEADER})

        set_source_files_properties(${MIDL_OUTPUT} PROPERTIES GENERATED TRUE)
        list(APPEND TARGET_OUTPUTS ${MIDL_OUTPUT})

    endforeach()

    foreach(idl_file ${idl_files_no_proxy})

        cmake_path(GET idl_file STEM IDL_NAME)

        set(IDL_HEADER ${OUTPUT_DIR}/${IDL_NAME}.h)

        add_custom_command(
            OUTPUT ${IDL_HEADER}
            COMMAND midl /nologo /target NT100 /env "${IDL_ENV}" /Zp8 /char unsigned /ms_ext /c_ext /h ${IDL_HEADER} /iid nul /proxy nul /dlldata nul ${idl_file} ${IDL_DEFINITIONS}
            WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
            DEPENDS ${idl_file}
            MAIN_DEPENDENCY ${idl_file}
            VERBATIM
        )

        set_source_files_properties(${IDL_HEADER} PROPERTIES GENERATED TRUE)
        list(APPEND TARGET_OUTPUTS ${IDL_HEADER})

    endforeach()

    # Touch the stamp file so Visual Studio's incremental build can track the
    # target as up-to-date. 
    add_custom_target(${target}
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}
        DEPENDS ${TARGET_OUTPUTS}
        SOURCES ${idl_files_with_proxy} ${idl_files_no_proxy})

endfunction()