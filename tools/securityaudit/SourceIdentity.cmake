# Copyright (C) Microsoft Corporation. All rights reserved.

set(audit_source_files
    src/linux/securityaudit/Collector.cpp
    src/linux/securityaudit/Collector.h
    src/linux/securityaudit/CollectorMain.cpp
    src/linux/securityaudit/AuditBuildInfo.h.in
    src/shared/securityaudit/ObservationSchema.h
    src/shared/securityaudit/ProcessCreated.h
    src/shared/securityaudit/ProcessExecuted.h
    src/shared/securityaudit/Protocol.h
    src/shared/securityaudit/SecurityEvents.h
    tools/securityaudit/CMakeLists.txt
    tools/securityaudit/SourceIdentity.cmake
    tools/securityaudit/GenerateBuildInfo.cmake
    tools/securityaudit/StagePayload.cmake.in
    tools/securityaudit/VerifyElf.cmake
    tools/securityaudit/build-payload.sh)
list(SORT audit_source_files)
set(audit_source_identity "")
set(audit_source_json "{}")
foreach(source IN LISTS audit_source_files)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WSL_SOURCE_ROOT}/${source}")
    endif()
    file(READ "${WSL_SOURCE_ROOT}/${source}" content)
    string(REPLACE "\r\n" "\n" content "${content}")
    string(SHA256 hash "${content}")
    string(APPEND audit_source_identity "${hash}  ${source}\n")
    string(JSON audit_source_json SET "${audit_source_json}" "${source}" "\"${hash}\"")
endforeach()
string(SHA256 WSL_SECURITYAUDIT_SOURCE_DIGEST "${audit_source_identity}")
