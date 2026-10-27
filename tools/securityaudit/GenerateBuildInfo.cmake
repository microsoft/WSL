# Copyright (C) Microsoft Corporation. All rights reserved.

file(READ "${WSL_SECURITYAUDIT_LIBRARY_DIR}/audit-payload.json" audit_manifest)
string(JSON WSL_SECURITYAUDIT_LOADER_REVISION GET "${audit_manifest}" loaderRevision)
string(JSON audit_sdk_version GET "${audit_manifest}" sdkVersion)
if(NOT audit_sdk_version STREQUAL LINUXSDK_VERSION)
    message(FATAL_ERROR "Audit archives require SDK ${audit_sdk_version}; init uses ${LINUXSDK_VERSION}.")
endif()
set(WSL_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
include("${CMAKE_CURRENT_LIST_DIR}/SourceIdentity.cmake")
configure_file("${CMAKE_SOURCE_DIR}/src/linux/securityaudit/AuditBuildInfo.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/AuditBuildInfo.h" @ONLY)
