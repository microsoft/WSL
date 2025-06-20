function(add_mc target mc_file)

    cmake_path(GET mc_file STEM MC_NAME)

    set(OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}\\${TARGET_PLATFORM}\\${CMAKE_BUILD_TYPE})
    set(RC_FILE ${OUTPUT_DIR}/${MC_NAME}.rc)
    set(HEADER_FILE ${OUTPUT_DIR}/${MC_NAME}.h)

    add_custom_command(
        OUTPUT ${RC_FILE} ${HEADER_FILE} ${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/${target}
        COMMAND mc.exe -A -b -c -h ${OUTPUT_DIR} -r ${OUTPUT_DIR} ${mc_file}
        COMMAND rc.exe -nologo -fo${OUTPUT_DIR}\\${MC_NAME}.res ${RC_FILE}
        COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/${target}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        DEPENDS ${mc_file}
        MAIN_DEPENDENCY ${mc_file}
        VERBATIM
    )

    add_custom_target(${target} DEPENDS ${RC_FILE} SOURCES ${mc_file})

    set_source_files_properties(${RC_FILE} PROPERTIES GENERATED TRUE)
endfunction()