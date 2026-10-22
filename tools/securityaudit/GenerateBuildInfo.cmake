# Copyright (C) Microsoft Corporation. All rights reserved.

execute_process(COMMAND git -c "safe.directory=${WSL_AUDIT_EBPF_SOURCE_ROOT}" rev-parse HEAD WORKING_DIRECTORY "${WSL_AUDIT_EBPF_SOURCE_ROOT}"
    OUTPUT_VARIABLE WSL_SECURITYAUDIT_LOADER_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
include("${CMAKE_CURRENT_LIST_DIR}/SourceIdentity.cmake")
configure_file("${WSL_SOURCE_ROOT}/src/linux/securityaudit/AuditBuildInfo.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/AuditBuildInfo.h" @ONLY)
