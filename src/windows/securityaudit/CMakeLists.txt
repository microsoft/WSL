set(AUDIT_MESSAGES_DIR ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE})
file(MAKE_DIRECTORY ${AUDIT_MESSAGES_DIR})
# Do not use add_mc's -c flag: it changes the existing experimental event identifiers.
add_custom_command(
    OUTPUT ${AUDIT_MESSAGES_DIR}/WindowsSecurityMessages.rc ${AUDIT_MESSAGES_DIR}/WindowsSecurityMessages.h
    BYPRODUCTS ${AUDIT_MESSAGES_DIR}/MSG00409.bin
    COMMAND mc.exe -h ${AUDIT_MESSAGES_DIR} -r ${AUDIT_MESSAGES_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/WindowsSecurityMessages.mc
    DEPENDS WindowsSecurityMessages.mc
    VERBATIM)
add_custom_target(wsl_securityaudit_messages DEPENDS
    ${AUDIT_MESSAGES_DIR}/WindowsSecurityMessages.rc ${AUDIT_MESSAGES_DIR}/WindowsSecurityMessages.h)

add_library(wsl_securityaudit_windows STATIC
    NativeAudit.cpp
    WindowsBridge.cpp
    WindowsSecuritySink.cpp
    SecuritySource.cpp)
add_dependencies(wsl_securityaudit_windows wsl_securityaudit_messages)
target_include_directories(wsl_securityaudit_windows PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE})
target_link_libraries(wsl_securityaudit_windows PUBLIC wsl_securityaudit_protocol
    PRIVATE common Authz.lib Advapi32.lib Ole32.lib Ws2_32.lib)
target_precompile_headers(wsl_securityaudit_windows PRIVATE precomp.h)
set_target_properties(wsl_securityaudit_windows PROPERTIES FOLDER windows)

# Stable resource-only module, retained by MSI so historical Security records remain readable.
add_library(wsl_securityaudit_resources SHARED
    ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/WindowsSecurityMessages.rc)
add_dependencies(wsl_securityaudit_resources wsl_securityaudit_messages)
set_target_properties(wsl_securityaudit_resources PROPERTIES
    LINKER_LANGUAGE CXX LINK_FLAGS "/NOENTRY" OUTPUT_NAME "wslsecurityaudit" FOLDER windows)
