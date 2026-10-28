# Copyright (C) Microsoft Corporation. All rights reserved.

add_subdirectory("${CMAKE_SOURCE_DIR}/src/linux/securityaudit" "${CMAKE_BINARY_DIR}/src/linux/securityaudit")

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
