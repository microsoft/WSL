/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Security.h

Abstract:

    This file contains user security function declarations.

--*/

#pragma once

#include <xstring>
#include "wrl/client.h"
#include "wil/resource.h"

namespace Security {

/// <summary>
/// Initializes the job object for a instance.
/// </summary>
void InitializeInstanceJob(_In_ HANDLE jobHandle);

/// <summary>
/// Returns true if the provided token is a member of the local administrators group
/// </summary>
bool IsTokenLocalAdministrator(_In_ HANDLE token);
} // namespace Security
