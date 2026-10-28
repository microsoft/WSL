# Copyright (C) Microsoft Corporation. All rights reserved.

set(messages_dir "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}")
file(MAKE_DIRECTORY "${messages_dir}")
add_custom_command(
    OUTPUT "${messages_dir}/WindowsSecurityMessages.rc" "${messages_dir}/WindowsSecurityMessages.h"
    BYPRODUCTS "${messages_dir}/MSG00409.bin"
    COMMAND mc.exe -h "${messages_dir}" -r "${messages_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/WindowsSecurityMessages.mc"
    DEPENDS WindowsSecurityMessages.mc
    VERBATIM)

add_library(securityaudit STATIC
    PluginRuntime.cpp SecuritySource.cpp WindowsBridge.cpp WindowsSecuritySink.cpp WslcSupport.cpp
    "${messages_dir}/WindowsSecurityMessages.h")
target_include_directories(securityaudit PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src/shared/securityaudit"
    PRIVATE "${messages_dir}")
target_precompile_headers(securityaudit REUSE_FROM common)
add_dependencies(securityaudit localization)
target_link_libraries(securityaudit PUBLIC common ${COMMON_LINK_LIBRARIES} advapi32 ws2_32 ole32 authz delayimp)
target_link_options(securityaudit INTERFACE "/DELAYLOAD:authz.dll")

add_library(wslsecurityauditplugin SHARED WindowsPlugin.cpp)
target_link_libraries(wslsecurityauditplugin PRIVATE securityaudit)
target_precompile_headers(wslsecurityauditplugin REUSE_FROM common)

add_library(wslsecurityauditmessages SHARED "${messages_dir}/WindowsSecurityMessages.rc")
set_target_properties(wslsecurityauditmessages PROPERTIES LINKER_LANGUAGE C)
target_link_options(wslsecurityauditmessages PRIVATE /NOENTRY /NODEFAULTLIB /INCREMENTAL:NO)
set_target_properties(securityaudit wslsecurityauditplugin wslsecurityauditmessages PROPERTIES FOLDER windows)
