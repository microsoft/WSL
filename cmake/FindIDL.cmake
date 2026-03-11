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

    foreach(idl_file ${idl_files_with_proxy})

        cmake_path(GET idl_file STEM IDL_NAME)

        set(IDL_HEADER ${OUTPUT_DIR}/${IDL_NAME}.h)

        # Adding a _${TARGET_PLATFORM} to work around object files having
        # the same paths regardless of TARGET_PLATFORM, which can cause the linker to fail with:
        # "fatal error LNK1112: module machine type 'x64' conflicts with target machine type 'ARM64'"
        set(IDL_I ${OUTPUT_DIR}/${IDL_NAME}_i_${TARGET_PLATFORM}.c)
        set(IDL_P ${OUTPUT_DIR}/${IDL_NAME}_p_${TARGET_PLATFORM}.c)
        set(IDL_DLLDATA ${OUTPUT_DIR}/dlldata_${TARGET_PLATFORM}.c)
        set(MIDL_OUTPUT ${IDL_HEADER} ${IDL_I} ${IDL_P} ${IDL_DLLDATA})

        add_custom_command(
            OUTPUT ${MIDL_OUTPUT} ${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/${target}
            COMMAND midl /nologo /target NT100 /env "${IDL_ENV}" /Zp8 /char unsigned /ms_ext /c_ext /h ${IDL_HEADER} /iid ${IDL_I} /proxy ${IDL_P} /dlldata ${IDL_DLLDATA} ${idl_file} ${IDL_DEFINITIONS}
            COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/${target}"
            WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
            DEPENDS ${idl_file}
            MAIN_DEPENDENCY ${idl_file}
            VERBATIM
        )

        set_source_files_properties(${MIDL_OUTPUT} PROPERTIES GENERATED TRUE)
        list(APPEND TARGET_OUTPUTS ${MIDL_OUTPUT})

    endforeach()

    foreach(idl_file ${idl_files_no_proxy})

        cmake_path(GET idl_file STEM IDL_NAME)

        set(IDL_HEADER ${OUTPUT_DIR}/${IDL_NAME}.h)

        add_custom_command(
            OUTPUT ${IDL_HEADER} ${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/${target}
            COMMAND midl /nologo /target NT100 /env "${IDL_ENV}" /Zp8 /char unsigned /ms_ext /c_ext /h ${IDL_HEADER} ${idl_file} ${IDL_DEFINITIONS}
            COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/${target}"
            WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
            DEPENDS ${idl_file}
            MAIN_DEPENDENCY ${idl_file}
            VERBATIM
        )

        set_source_files_properties(${IDL_HEADER} PROPERTIES GENERATED TRUE)
        list(APPEND TARGET_OUTPUTS ${IDL_HEADER})

    endforeach()

    add_custom_target(${target} DEPENDS ${TARGET_OUTPUTS} SOURCES ${idl_files_with_proxy} ${idl_files_no_proxy})

endfunction()