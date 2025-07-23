/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslCoreConfigInterface.h

Abstract:

    This file contains the WSL Core Config Interface class interface declaration.

--*/

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <basetyps.h>

enum WslConfigEntry
{
    NoEntry,
    ProcessorCount,
    MemorySizeBytes,
    SwapSizeBytes,
    SwapFilePath,
    VhdSizeBytes,
    Networking,
    FirewallEnabled,
    IgnoredPorts,
    LocalhostForwardingEnabled,
    HostAddressLoopbackEnabled,
    AutoProxyEnabled,
    InitialAutoProxyTimeout,
    DNSProxyEnabled,
    DNSTunnelingEnabled,
    BestEffortDNSParsingEnabled,
    AutoMemoryReclaim,
    GUIApplicationsEnabled,
    NestedVirtualizationEnabled,
    SafeModeEnabled,
    SparseVHDEnabled,
    VMIdleTimeout,
    DebugConsoleEnabled,
    HardwarePerformanceCountersEnabled,
    KernelPath,
    SystemDistroPath,
    KernelModulesPath,
};

enum NetworkingConfiguration
{
    None = 0,
    Nat = 1,
    Bridged = 2,
    Mirrored = 3,
    VirtioProxy = 4
};

enum MemoryReclaimConfiguration
{
    Disabled = 0,
    Gradual = 1,
    DropCache = 2
};

typedef struct WslConfig* WslConfig_t;

struct WslConfigSetting
{
    enum WslConfigEntry ConfigEntry;
    union
    {
        const wchar_t* StringValue;
        unsigned __int64 UInt64Value;
        int Int32Value;
        bool BoolValue;
        enum NetworkingConfiguration NetworkingConfigurationValue;
        enum MemoryReclaimConfiguration MemoryReclaimModeValue;
    };
};

STDAPI_(const wchar_t*)
GetWslConfigFilePath();

STDAPI_(WslConfig_t)
CreateWslConfig(const wchar_t* wslConfigFilePath);

STDAPI_(void)
FreeWslConfig(WslConfig_t wslConfig);

STDAPI_(struct WslConfigSetting)
GetWslConfigSetting(WslConfig_t wslConfig, enum WslConfigEntry ConfigEntry);

STDAPI_(unsigned long)
SetWslConfigSetting(WslConfig_t wslConfig, struct WslConfigSetting setting);