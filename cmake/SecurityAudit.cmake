# Copyright (C) Microsoft Corporation. All rights reserved.

set(SECURITYAUDIT_PAYLOAD_DIR "" CACHE PATH "Linux CI audit payload for TARGET_PLATFORM")
if(NOT EXISTS "${SECURITYAUDIT_PAYLOAD_DIR}/payload.json")
    message(FATAL_ERROR
        "Auditing requires -DSECURITYAUDIT_PAYLOAD_DIR=<Linux CI payload>. "
        "See doc/docs/security-audit.md. Use -DWSL_BUILD_SECURITY_AUDIT=OFF only for an explicitly unaudited development build.")
endif()
set(validate_audit_command powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass
    -File "${CMAKE_SOURCE_DIR}/tools/securityaudit/Validate-Payload.ps1"
    -PayloadDirectory "${SECURITYAUDIT_PAYLOAD_DIR}" -Architecture "${TARGET_PLATFORM}" -SourceDirectory "${CMAKE_SOURCE_DIR}")
execute_process(COMMAND ${validate_audit_command} COMMAND_ERROR_IS_FATAL ANY)
file(GLOB_RECURSE SECURITYAUDIT_PAYLOAD_FILES CONFIGURE_DEPENDS "${SECURITYAUDIT_PAYLOAD_DIR}/*")
add_custom_target(securityauditpayload COMMAND ${validate_audit_command} VERBATIM)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SECURITYAUDIT_PAYLOAD_DIR}/payload.json")

set(SECURITYAUDIT_DIRECTORY "<Directory Id=\"SECURITYAUDITFOLDER\" Name=\"securityaudit\">")
foreach(directory "" runtime licenses)
    if(NOT directory STREQUAL "")
        string(APPEND SECURITYAUDIT_DIRECTORY "<Directory Id=\"AUDIT_${directory}\" Name=\"${directory}\">")
    endif()
    foreach(path IN LISTS SECURITYAUDIT_PAYLOAD_FILES)
        file(RELATIVE_PATH relative "${SECURITYAUDIT_PAYLOAD_DIR}" "${path}")
        get_filename_component(parent "${relative}" DIRECTORY)
        if(NOT parent STREQUAL directory)
            continue()
        endif()
        get_filename_component(name "${relative}" NAME)
        string(SHA256 id "${relative}")
        string(SUBSTRING "${id}" 0 24 id)
        string(REPLACE "&" "&amp;" xml_path "${path}")
        string(APPEND SECURITYAUDIT_DIRECTORY
            "<Component Id=\"audit_${id}\" Guid=\"*\" Bitness=\"always64\">"
            "<File Id=\"audit_${id}\" Name=\"${name}\" Source=\"${xml_path}\" KeyPath=\"yes\" /></Component>")
        string(APPEND SECURITYAUDIT_COMPONENT_REFS "<ComponentRef Id=\"audit_${id}\" />")
    endforeach()
    if(NOT directory STREQUAL "")
        string(APPEND SECURITYAUDIT_DIRECTORY "</Directory>")
    endif()
endforeach()
string(APPEND SECURITYAUDIT_DIRECTORY "</Directory>")
