# Copyright (C) Microsoft Corporation. All rights reserved.

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SECURITYAUDIT_SOURCE_DIR}/metadata/build.json"
    "${SECURITYAUDIT_SOURCE_DIR}/metadata/link.json")
file(READ "${SECURITYAUDIT_SOURCE_DIR}/metadata/build.json" audit_manifest)
string(JSON WSL_SECURITYAUDIT_LOADER_REVISION GET "${audit_manifest}" source_commit)
string(JSON audit_sdk GET "${audit_manifest}" sdk)
if(NOT audit_sdk STREQUAL "Microsoft.WSL.LinuxSdk/${LINUXSDK_VERSION}")
    message(FATAL_ERROR "Audit archives require ${audit_sdk}; init uses Microsoft.WSL.LinuxSdk/${LINUXSDK_VERSION}.")
endif()
set(WSL_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
include("${CMAKE_CURRENT_LIST_DIR}/SourceIdentity.cmake")
configure_file("${CMAKE_SOURCE_DIR}/src/linux/securityaudit/AuditBuildInfo.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/AuditBuildInfo.h" @ONLY)
