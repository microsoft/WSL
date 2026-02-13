/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Errors.h

Abstract:

    Header file for Errors.

--*/
#pragma once
#include <windows.h>

// This is a placeholder facility code for Container Command 1nterface (CC1).
// This should be replaced in the future when a proper facility code is assigned.
#define WSLC_CLI_ERROR_FACILITY 0xCC1

#define WSLC_CLI_ERROR_INTERNAL_ERROR                       ((HRESULT)0x8CC10001)
#define WSLC_CLI_ERROR_INVALID_CL_ARGUMENTS                 ((HRESULT)0x8CC10002)
#define WSLC_CLI_ERROR_COMMAND_FAILED                       ((HRESULT)0x8CC10003)
#define WSLC_CLI_ERROR_COMMAND_REQUIRES_ADMIN               ((HRESULT)0x8CC10004)
