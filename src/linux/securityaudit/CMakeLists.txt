# Copyright (C) Microsoft Corporation. All rights reserved.

set(WSL_SECURITYAUDIT_PAYLOAD_DIR "" CACHE PATH
    "Architecture-specific static audit payload built by tools/securityaudit/build-payload.sh")
set(WSL_SECURITYAUDIT_STAGED_PAYLOAD "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/securityaudit")
set(WSL_SECURITYAUDIT_STAGED_PAYLOAD "${WSL_SECURITYAUDIT_STAGED_PAYLOAD}" PARENT_SCOPE)

# Validate on every build: an updated dependency package or source must never reuse a stale collector.
add_custom_target(wsl_audit_collector
    COMMAND powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive
        -File "${CMAKE_SOURCE_DIR}/tools/securityaudit/Validate-Payload.ps1"
        -PayloadDirectory "${WSL_SECURITYAUDIT_PAYLOAD_DIR}"
        -Architecture "${LLVM_ARCH}"
        -SourceRoot "${CMAKE_SOURCE_DIR}"
        -StageDirectory "${WSL_SECURITYAUDIT_STAGED_PAYLOAD}"
    BYPRODUCTS "${WSL_SECURITYAUDIT_STAGED_PAYLOAD}/wsl-audit-collector"
               "${WSL_SECURITYAUDIT_STAGED_PAYLOAD}/audit-payload.json"
    VERBATIM)
set_target_properties(wsl_audit_collector PROPERTIES FOLDER linux)
