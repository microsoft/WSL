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
    GetUsername,
    WslcSuccess,
    WslcSessionRejected,
    WslcContainerRejected,
    WslcImagePull,
    WslcVmRestart,
    WslcVmStopCommitted,
    WslcVmNeverStarted,
    MountFolderAccess,
    WslcVmExitDuringSessionRelease,
    CrashOnLoad,
    CrashOnVmStart,
    CrashOnDistributionStart,
    CrashOnVmStop,
    ConcurrentCallbacks,
    AsyncCallback,
    WslcCrossApartmentProcess,
    ConcurrentRegistration
};

constexpr auto c_testType = L"TestType";
constexpr auto c_logFile = L"LogFile";
constexpr auto c_mountFolder = L"MountFolder";
constexpr auto c_hostPid = L"HostPid";
constexpr auto c_callbackResult = L"CallbackResult";
constexpr auto c_asyncCallbackStart = L"Global\\WslPluginAsyncCallback-start";
constexpr auto c_asyncCallbackDone = L"Global\\WslPluginAsyncCallback-done";
constexpr auto c_registrationEntered = L"Global\\WslPluginRegistration-entered";
constexpr auto c_registrationRelease = L"Global\\WslPluginRegistration-release";
constexpr auto c_vmStoppingEnteredEvent = L"Global\\WslcVmExitDuringSessionRelease-entered";
constexpr auto c_vmStoppingReleaseEvent = L"Global\\WslcVmExitDuringSessionRelease-release";

inline wil::unique_hkey OpenTestRegistryKey(REGSAM AccessMask)
{
    return wsl::windows::common::registry::CreateKey(HKEY_LOCAL_MACHINE, c_configKey, AccessMask, nullptr, REG_OPTION_VOLATILE);
}
