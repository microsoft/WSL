function(add_appx_target target binaries manifest_in output_package dependencies)

    set(PACKAGE_LAYOUT "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PLATFORM}/package_layout")
    set(MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.xml")
    set(PRI_CONF "${CMAKE_CURRENT_BINARY_DIR}/priconf.xml")
    set(OUTPUT_RESOURCES_PRI "${CMAKE_CURRENT_BINARY_DIR}/resources.pri")

    # generate the list of languages for the appxmanifest
    set(SUPPORTED_LANGS_MANIFEST_ENTRIES "")
    foreach(LANG ${SUPPORTED_LANGS})
        string(TOUPPER "${LANG}" LANG)
        set(SUPPORTED_LANGS_MANIFEST_ENTRIES "${SUPPORTED_LANGS_MANIFEST_ENTRIES}\n    <Resource Language=\"${LANG}\"/>")
    endforeach()

    configure_file(${manifest_in} ${MANIFEST})

    file(MAKE_DIRECTORY ${PACKAGE_LAYOUT})

    # images
    file(MAKE_DIRECTORY ${PACKAGE_LAYOUT}/Images)
    set(RESOURCES_DEPENDENCY)
    file(GLOB IMAGES RELATIVE ${PROJECT_SOURCE_DIR}/ "${PROJECT_SOURCE_DIR}/images/*.png")
    foreach(e ${IMAGES})
        file(CREATE_LINK ${PROJECT_SOURCE_DIR}/${e} ${PACKAGE_LAYOUT}/${e} SYMBOLIC)
        list(APPEND RESOURCES_DEPENDENCY ${PROJECT_SOURCE_DIR}/${e})
    endforeach()

    # Localization. These files are used by makepri to generate resources.pri,
    # but they aren't added to the package as individual files.
    if (IS_SYMLINK "${PACKAGE_LAYOUT}/Strings")
        file(REMOVE "${PACKAGE_LAYOUT}/Strings")
    endif()
    file(MAKE_DIRECTORY ${PACKAGE_LAYOUT}/Strings)

    foreach(LANG ${SUPPORTED_LANGS})
        set(LANGUAGE_RESOURCE_DIRECTORY ${PACKAGE_LAYOUT}/Strings/${LANG})
        set(PACKAGED_LANGUAGE_RESOURCE ${LANGUAGE_RESOURCE_DIRECTORY}/Resources.resw)
        if (LANG STREQUAL "en-US")
            set(LANGUAGE_RESOURCE ${LOCALIZATION_EN_US_RESW})
        else()
            set(LANGUAGE_RESOURCE ${PROJECT_SOURCE_DIR}/localization/strings/${LANG}/Resources.resw)
        endif()

        add_custom_command(
            OUTPUT ${PACKAGED_LANGUAGE_RESOURCE}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${LANGUAGE_RESOURCE_DIRECTORY}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LANGUAGE_RESOURCE}" "${PACKAGED_LANGUAGE_RESOURCE}"
            DEPENDS ${LANGUAGE_RESOURCE}
            VERBATIM
        )
        list(APPEND RESOURCES_DEPENDENCY ${PACKAGED_LANGUAGE_RESOURCE})
    endforeach()

    foreach(binary ${binaries})
    set(BINARY_SRC "${BIN}/${binary}")
    set(BINARY_DEST "${PACKAGE_LAYOUT}/${binary}")
    add_custom_command(
        OUTPUT ${BINARY_DEST}
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${BINARY_SRC}" "${BINARY_DEST}"
        DEPENDS ${BINARY_SRC}
    )
    list(APPEND BINARIES_DEPENDENCY ${BINARY_DEST})
    endforeach()

    # Reduce the output of makeappx unless WSL_APPX_DEBUG is set to make the build output nicer to read
    if (WSL_SILENT_APPX_BUILD)
        set(COMMAND_SUFFIX "2>NUL;>;NUL")
    endif ()

    # generate priconf.xml
    string(REPLACE ";" "_" SUPPORTED_LANGS_STR "${SUPPORTED_LANGS}")
    add_custom_command(
        OUTPUT ${PRI_CONF}
        COMMAND makepri.exe createconfig /cf ${PRI_CONF} /dq ${SUPPORTED_LANGS_STR} /pv 10.0 /o ${COMMAND_SUFFIX}
        COMMAND_EXPAND_LISTS
    )

    # generate resources.pri
    add_custom_command(
        OUTPUT ${OUTPUT_RESOURCES_PRI} ${CMAKE_CURRENT_BINARY_DIR}/resources.map.txt
        COMMAND makepri.exe new /pr ${PACKAGE_LAYOUT} /cf ${PRI_CONF} /of ${OUTPUT_RESOURCES_PRI} /mn ${MANIFEST} /mf AppX /o /IndexOptions +lf ${COMMAND_SUFFIX}
        COMMAND_EXPAND_LISTS
        DEPENDS ${PRI_CONF} ${MANIFEST} ${BINARIES_DEPENDENCY} ${RESOURCES_DEPENDENCY} # Make sure the package is rebuilt if any of the resources change
    )

    # make appx
    add_custom_command(
        OUTPUT ${output_package}
        COMMAND makeappx.exe pack /m ${MANIFEST} /f ${CMAKE_CURRENT_BINARY_DIR}/resources.map.txt /p ${output_package} /o ${COMMAND_SUFFIX}
        COMMAND ${PACKAGE_SIGN_COMMAND} ${output_package} ${COMMAND_SUFFIX}
        COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/${target}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMAND_EXPAND_LISTS
        DEPENDS ${MANIFEST} ${BINARIES_DEPENDENCY} ${OUTPUT_RESOURCES_PRI} ${CMAKE_CURRENT_BINARY_DIR}/resources.map.txt # Make sure the package is rebuilt if any of the binaries or resources change
    )

    add_custom_target(${target} DEPENDS ${output_package})
    add_dependencies(${target} localization)

    foreach(e ${dependencies})
        add_dependencies(${target} ${e})
    endforeach()

    set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL FALSE SOURCES ${manifest_in})

    set_source_files_properties(${output_package} PROPERTIES GENERATED TRUE)
endfunction()