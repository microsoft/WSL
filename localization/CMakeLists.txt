set(LOCALIZATION_PS1 ${PROJECT_SOURCE_DIR}/tools/generateLocalizationHeader.ps1)
set(LOCALIZATION_MERGE_PS1 ${PROJECT_SOURCE_DIR}/tools/mergeLocalizationResources.ps1)
set(WSL_EN_US_RESW ${CMAKE_CURRENT_LIST_DIR}/strings/en-US/WSL.resw)
set(WSLC_EN_US_RESW ${CMAKE_CURRENT_LIST_DIR}/strings/en-US/WSLC.resw)
set(MESSAGESHEAD ${GENERATED_DIR}/Localization.h)
file(GLOB TRANSLATED_RESOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/strings/*/Resources.resw)

add_custom_command(
    OUTPUT ${LOCALIZATION_EN_US_RESW}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_DIR}/localization/strings/en-US"
    COMMAND powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive -File "${LOCALIZATION_MERGE_PS1}"
        -WslResource "${WSL_EN_US_RESW}"
        -WslcResource "${WSLC_EN_US_RESW}"
        -OutFile "${LOCALIZATION_EN_US_RESW}"
    DEPENDS ${WSL_EN_US_RESW} ${WSLC_EN_US_RESW} ${LOCALIZATION_MERGE_PS1}
    VERBATIM
)

add_custom_command(
    OUTPUT ${MESSAGESHEAD} ${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/localization
    COMMAND powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive -File "${LOCALIZATION_PS1}"
        -OutFile "${MESSAGESHEAD}"
        -DefaultResourceFile "${LOCALIZATION_EN_US_RESW}"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/localization"
    DEPENDS ${LOCALIZATION_EN_US_RESW} ${TRANSLATED_RESOURCES} ${LOCALIZATION_PS1}
    VERBATIM
)

add_custom_target(
    localization
    DEPENDS ${MESSAGESHEAD} ${LOCALIZATION_EN_US_RESW}
    SOURCES ${WSL_EN_US_RESW} ${WSLC_EN_US_RESW} ${LOCALIZATION_PS1} ${LOCALIZATION_MERGE_PS1}
)