set(DEPENDENCIES "wsl.exe;wslinstaller.exe;wslinstallerproxystub.dll")

# Don't include the MSI if building a thin package.
if (NOT "${WSL_BUILD_THIN_PACKAGE}")
    list(APPEND DEPENDENCIES "wsl.msi")
endif()

add_appx_target(msixinstallerpackage
                "${DEPENDENCIES}"
                "${CMAKE_CURRENT_LIST_DIR}/AppxManifest.in"
                "${BIN}/installer.msix"
                "wsl;wslinstaller;msipackage;wslinstallerproxystub")

set(ARM64_BUNDLE_PATH ${CMAKE_SOURCE_DIR}/bin/arm64/${CMAKE_BUILD_TYPE}/installer.msix)
set(X64_BUNDLE_PATH ${CMAKE_SOURCE_DIR}/bin/X64/${CMAKE_BUILD_TYPE}/installer.msix)

# Only build the bundle if it exists
if ((TARGET_PLATFORM STREQUAL "arm64" AND EXISTS ${X64_BUNDLE_PATH}) OR (TARGET_PLATFORM STREQUAL "x64" AND EXISTS ${ARM64_BUNDLE_PATH}))
    set(PACKAGE_NAME "Microsoft.WSL_${PACKAGE_VERSION}")
    set(BUNDLE_DIR ${CMAKE_SOURCE_DIR}/bundle/${CMAKE_BUILD_TYPE})
    set(BUNDLE_MAPPINGS_FILE ${BUNDLE_DIR}/mappings.ini)

    set(OUTPUT_BUNDLE ${BUNDLE_DIR}/${PACKAGE_NAME}_x64_ARM64.msixbundle)
    file(MAKE_DIRECTORY ${BUNDLE_DIR})

    file(WRITE ${BUNDLE_MAPPINGS_FILE} "[Files]\n")
    file(APPEND ${BUNDLE_MAPPINGS_FILE} "\"${X64_BUNDLE_PATH}\"" " \"${PACKAGE_NAME}_x64.msix\"\n")
    file(APPEND ${BUNDLE_MAPPINGS_FILE} "\"${ARM64_BUNDLE_PATH}\"" " \"${PACKAGE_NAME}_ARM64.msix\"\n")

    add_custom_command(
        OUTPUT ${OUTPUT_BUNDLE}
        COMMAND makeappx.exe bundle -f ${BUNDLE_MAPPINGS_FILE} -bv ${PACKAGE_VERSION} -o -p ${OUTPUT_BUNDLE}
        COMMAND ${PACKAGE_SIGN_COMMAND} ${OUTPUT_BUNDLE}
        COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/bundle"
        VERBATIM
        DEPENDS ${CMAKE_SOURCE_DIR}/bin/X64/${CMAKE_BUILD_TYPE}/installer.msix ${CMAKE_SOURCE_DIR}/bin/arm64/${CMAKE_BUILD_TYPE}/installer.msix
    )

    set_source_files_properties(${OUTPUT_BUNDLE} PROPERTIES GENERATED TRUE)
    add_custom_target(bundle DEPENDS ${OUTPUT_BUNDLE})
    add_dependencies(bundle msixinstallerpackage)
    set_target_properties(bundle PROPERTIES EXCLUDE_FROM_ALL FALSE)
endif()