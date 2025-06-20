/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PluginTests.h

Abstract:

    This file contains shared definitions used in the plugin tests.

--*/

#pragma once

#include "registry.hpp"
#include <wil/resource.h>
constexpr auto c_configKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Lxss\\Test";

enum class PluginTestType
{
    Invalid,
    Success,
    FailToLoad,
    FailToStartVm,
    FailToStartDistro,
    FailToStopVm,
    FailToStopDistro,
    ApiErrors,
    PluginError,
    PluginRequiresUpdate,
    SameDistroId,
    ErrorMessageStartVm,
    ErrorMessageStartDistro,
    FailToStartVmWithPluginErrorMessage,
    InitPidIsDifferent,
    FailToRegisterUnregisterDistro,
    RunDistroCommand,
    GetUsername
};

constexpr auto c_testType = L"TestType";
constexpr auto c_logFile = L"LogFile";

inline wil::unique_hkey OpenTestRegistryKey(REGSAM AccessMask)
{
    return wsl::windows::common::registry::CreateKey(HKEY_LOCAL_MACHINE, c_configKey, AccessMask, nullptr, REG_OPTION_VOLATILE);
}