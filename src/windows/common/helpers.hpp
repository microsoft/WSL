/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    helpers.hpp

Abstract:

    This file contains helper function declarations.

--*/

#pragma once

#include <winsock2.h>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <lxcoreapi.h>
#include <gsl/gsl>
#include <wil/com.h>
#include <wil/filesystem.h>
#include <wil/result.h>
#include <winternl.h>
#include "lxinitshared.h"

#define _1KB ((UINT64)(1024))
#define _1MB (_1KB * _1KB)
#define _1GB (_1KB * _1MB)

#define LXSS_LAUNCH_FLAG_ENABLE_INTEROP 0x1
#define LXSS_LAUNCH_FLAG_TRANSLATE_ENVIRONMENT 0x2
#define LXSS_LAUNCH_FLAG_USE_SYSTEM_DISTRO 0x4
#define LXSS_LAUNCH_FLAG_SHELL_LOGIN 0x8

#define LXSS_IS_WHITESPACE(_Char) (((_Char) == L' ') || ((_Char) == L'\t'))

#define LXSS_ROOTFS_DIRECTORY L"rootfs"
#define LXSS_TEMP_DIRECTORY L"temp"

#define CONTINUE_IF_FAILED(x) \
    { \
        if (FAILED_LOG((x))) \
        { \
            continue; \
        } \
    }

#define CONTINUE_IF_FAILED_WIN32(x) \
    { \
        if (FAILED_WIN32_LOG((x))) \
        { \
            continue; \
        } \
    }

namespace wsl::windows::common::helpers {

enum class LaunchWslRelayFlags
{
    None = 0,
    DisableTelemetry = 1,
    HideWindow = 2,
    ConnectPipe = 4
};

DEFINE_ENUM_FLAG_OPERATORS(LaunchWslRelayFlags);

enum WindowsBuildNumbers : ULONG
{
    Vibranium = 19041,
    Vibranium_20H2 = 19042,
    Vibranium_21H1 = 19043,
    Vibranium_21H2 = 19044,
    Vibranium_22H2 = 19045,
    Iron = 20348,
    Cobalt = 22000,
    Nickel = 22621,
    Nickel_23H2 = 22631,
    Zinc = 25398,
    Germanium = 26100,
};

struct GuidLess
{
    bool operator()(REFGUID left, REFGUID right) const
    {
        return memcmp(&left, &right, sizeof(GUID)) < 0;
    }
};

typedef wil::unique_any_handle_null<decltype(&::ClosePseudoConsole), ::ClosePseudoConsole> unique_pseudo_console;

using unique_environment_block = wil::unique_any<LPVOID, decltype(&DestroyEnvironmentBlock), DestroyEnvironmentBlock>;

inline void DeleteProcThreadAttributeList(_In_ PPROC_THREAD_ATTRIBUTE_LIST AttributeList)
{
    ::DeleteProcThreadAttributeList(AttributeList);
    CoTaskMemFree(AttributeList);
}

using unique_proc_attribute_list =
    wil::unique_any<PPROC_THREAD_ATTRIBUTE_LIST, decltype(&DeleteProcThreadAttributeList), DeleteProcThreadAttributeList>;

using unique_environment_strings = wil::unique_any<LPTCH, decltype(&::FreeEnvironmentStrings), ::FreeEnvironmentStrings>;

using unique_pseudo_console = wil::unique_any_handle_null<decltype(&ClosePseudoConsole), ClosePseudoConsole>;

using unique_mta_cookie = wil::unique_any<CO_MTA_USAGE_COOKIE, decltype(::CoDecrementMTAUsage), &::CoDecrementMTAUsage>;

void ConnectPipe(_In_ HANDLE Pipe, _In_ DWORD Timeout = INFINITE, _In_ const std::vector<HANDLE>& ExitEvents = {});

std::wstring_view ConsumeArgument(_In_ std::wstring_view CommandLine, _In_ std::wstring_view Argument);

void CreateConsole(_In_ LPCWSTR ConsoleTitle = nullptr);

unique_proc_attribute_list CreateProcThreadAttributeList(_In_ DWORD AttributeCount);

std::vector<gsl::byte> GenerateConfigurationMessage(
    _In_ const std::wstring& DistributionName,
    _In_ ULONG FixedDrivesBitmap = 0,
    _In_ ULONG DefaultUid = LX_UID_ROOT,
    _In_ const std::string& Timezone = {},
    _In_ const std::wstring& Plan9SocketPath = {},
    _In_ ULONG FeatureFlags = 0,
    _In_ LX_INIT_DRVFS_MOUNT DrvfsMount = LxInitDrvfsMountElevated);

std::vector<gsl::byte> GenerateTimezoneUpdateMessage(_In_ std::string_view Timezone);

std::string GetLinuxTimezone(_In_opt_ HANDLE UserToken = nullptr);

std::wstring GetUniquePipeName();

struct WindowsVersion
{
    DWORD MajorVersion;
    DWORD MinorVersion;
    ULONG BuildNumber;
    DWORD UpdateBuildRevision;
};

WindowsVersion GetWindowsVersion();

std::string GetWindowsVersionString();

std::filesystem::path GetUserProfilePath(_In_opt_ HANDLE userToken = nullptr);

std::filesystem::path GetWslConfigPath(_In_opt_ HANDLE userToken = nullptr);

bool IsPackageInstalled(_In_ LPCWSTR PackageFamilyName);

bool IsServicePresent(_In_ LPCWSTR ServiceName);

bool IsWindows11OrAbove();

bool IsWslOptionalComponentPresent();

bool IsWslSupportInterfacePresent();

void LaunchDebugConsole(_In_ LPCWSTR PipeName, _In_ bool ConnectExistingPipe, _In_ HANDLE UserToken, _In_opt_ HANDLE LogFile, _In_ bool DisableTelemetry);

[[nodiscard]] wil::unique_handle LaunchInteropServer(
    _In_opt_ LPCGUID DistroId,
    _In_ HANDLE InteropHandle,
    _In_opt_ HANDLE EventHandle,
    _In_opt_ HANDLE ParentHandle,
    _In_opt_ LPCGUID VmId,
    _In_opt_ HANDLE UserToken = nullptr);

void LaunchKdRelay(_In_ LPCWSTR PipeName, _In_ HANDLE UserToken, _In_ int Port, _In_ HANDLE ExitEvent, _In_ bool DisableTelemetry);

void LaunchPortRelay(_In_ SOCKET Socket, _In_ const GUID& VmId, _In_ HANDLE UserToken, _In_ bool DisableTelemetry);

void LaunchWslSettingsOOBE(_In_ HANDLE UserToken);

std::wstring_view ParseArgument(_In_ std::wstring_view CommandLine, _In_ bool HandleQuotes = false);

bool ReopenStdHandles();

#ifdef _WIN64
INT64
RoundUpToNearestPowerOfTwo(_In_ INT64 Num);
#else
INT32
RoundUpToNearestPowerOfTwo(_In_ INT32 Num);
#endif

DWORD RunProcess(_Inout_ std::wstring& CommandLine);

void SetHandleInheritable(_In_ HANDLE Handle, _In_ bool Inheritable = true);

bool TryAttachConsole();

} // namespace wsl::windows::common::helpers
