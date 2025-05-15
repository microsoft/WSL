set(LOCALIZATION_PS1 ${PROJECT_SOURCE_DIR}/tools/generateLocalizationHeader.ps1)
set (MESSAGESHEAD ${GENERATED_DIR}/Localization.h)
add_custom_command(
    OUTPUT ${MESSAGESHEAD} ${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/localization
    COMMAND powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive  ${LOCALIZATION_PS1} -OutFile ${MESSAGESHEAD}
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/localization"
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/strings/en-US/Resources.resw ${LOCALIZATION_PS1} # Make sure the head file is rebuilt if any of the resources change
    VERBATIM
)

add_custom_target(localization DEPENDS ${MESSAGESHEAD} ${LOCALIZATION_PS1} SOURCES ${CMAKE_CURRENT_LIST_DIR}/strings/en-US/Resources.resw)