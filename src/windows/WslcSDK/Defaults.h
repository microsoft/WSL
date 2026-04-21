/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Defaults.h

Abstract:

    Contains default values for settings used in the WSL Container SDK.

--*/

#pragma once
#include <cstdint>

constexpr uint32_t s_DefaultCPUCount = 2;
constexpr uint32_t s_DefaultMemoryMB = 2000;
// Maximum value per use with HVSOCKET_CONNECT_TIMEOUT_MAX
constexpr ULONG s_DefaultBootTimeout = 300000;
// Default to 1 GB
constexpr UINT64 s_DefaultStorageSize = 1000 * 1000 * 1000;
