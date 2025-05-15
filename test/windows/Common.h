/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Common.h

Abstract:

    This file contains common definitions used for testing.

--*/

#pragma once

#include <WexTestClass.h>
#include <LogController.h>
#include <future>
#include <vector>
#include <string>
#include <thread>
#include <stdio.h>
#include <stdlib.h>
#include "precomp.h"
#include "lxsstest.h"
#include "wslutil.h"
#include "WslCoreConfig.h"

//
// N.B. This is also defined in 'lxtcommon.h' & 'lxsetup.ps1'. Update those
//      files too, if the distro name changes here.
//
#define LXSS_DISTRO_NAME_TEST "test_distro"
#define LXSS_DISTRO_NAME_TEST_L WIDEN(LXSS_DISTRO_NAME_TEST)

#define LXSST_REMOVE_DISTRO_CONF_COMMAND_LINE L"-u root -e rm /etc/wsl.conf"

#define WSL1_TEST_ONLY() \
    if (LxsstuVmMode()) \
    { \
        LogSkipped("This test is only applicable to WSL1"); \
        return; \
    }

#define WSL2_TEST_ONLY() \
    if (!LxsstuVmMode()) \
    { \
        LogSkipped("This test is only applicable to WSL2"); \
        return; \
    }

// macro for skipping tests that are currently failing due to not yet being fully implemented
#define SKIP_TEST_NOT_IMPL() \
    { \
        LogSkipped("This test is skipped; not yet fully implemented"); \
        return; \
    }

#define WINDOWS_11_TEST_ONLY() \
    if (!wsl::windows::common::helpers::IsWindows11OrAbove()) \
    { \
        LogSkipped("This test is only applicable to Windows 11 and above"); \
        return; \
    }

#define WSL_TEST_VERSION_REQUIRED(_version) \
    if (wsl::windows::common::helpers::GetWindowsVersion().BuildNumber < _version) \
    { \
        LogSkipped("This test requires Windows version %u or later", _version); \
        return; \
    }

#define SKIP_TEST_ARM64() \
    { \
        if constexpr (wsl::shared::Arm64) \
        { \
            LogSkipped("This test is skipped for ARM64"); \
            return; \
        } \
    }

#define SKIP_TEST_UNSTABLE() \
    { \
        LogSkipped("This test is skipped because it's unstable"); \
        return; \
    }

#define WSL_SETTINGS_TEST() \
    if constexpr (!WSL_BUILD_WSL_SETTINGS) \
    { \
        LogSkipped("This test is skipped wslsettings wasn't built"); \
        return; \
    }

#define WSL_TEST_CLASS(_name) \
    BEGIN_TEST_CLASS(_name) \
        TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"LxssManager.dll") \
        TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"LxssManagerProxyStub.dll") \
        TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"wslclient.dll") \
        TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"wslservice.exe") \
        TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"WslServiceProxyStub.dll") \
        TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"wslhost.exe") \
        TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"wslrelay.exe") \
        TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"wslconfig.exe") \
        TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"wsl.exe") \
        TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"wslg.exe") \
        TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"msrdc.exe") \
        TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"msal.wsl.proxy.exe") \
    END_TEST_CLASS()

//
// RAII Wrapper that prevents the UVM from timing out
//
class WslKeepAlive
{
public:
    WslKeepAlive(HANDLE Token = nullptr);

    ~WslKeepAlive();

    WslKeepAlive(const WslKeepAlive&) = delete;
    WslKeepAlive(WslKeepAlive&&) = delete;
    const WslKeepAlive& operator=(WslKeepAlive&&) = delete;
    const WslKeepAlive& operator=(WslKeepAlive&) = delete;

    void Set();

    void Run();

    void Reset();

private:
    wil::unique_handle m_write;
    wil::unique_handle m_read;
    std::thread m_thread;
    std::optional<std::promise<void>> m_running;
    HANDLE m_token = nullptr;
};

//
// RAII Wrapper for .wslconfig changes
//

class WslConfigChange
{
public:
    WslConfigChange(const std::wstring& Content);

    ~WslConfigChange();
    WslConfigChange(const WslConfigChange&) = delete;
    WslConfigChange(WslConfigChange&& other);
    const WslConfigChange& operator=(WslConfigChange&&) = delete;
    const WslConfigChange& operator=(WslConfigChange&) = delete;

    static std::wstring Update(const std::wstring& Content);

private:
    std::optional<std::wstring> m_originalContent;
};

template <typename T>
class RegistryKeyChange
{
public:
    RegistryKeyChange(HKEY Hive, LPCWSTR Key, LPCWSTR Name, const T& Value) : m_value(Name)
    {
        m_key = wsl::windows::common::registry::CreateKey(Hive, Key, KEY_ALL_ACCESS);

        m_originalValue = Get();

        Set(Value);
    }

    ~RegistryKeyChange()
    {
        if (m_key)
        {
            if (m_originalValue.has_value())
            {
                Set(m_originalValue.value());
            }
            else
            {
                wsl::windows::common::registry::DeleteKeyValue(m_key.get(), m_value.c_str());
            }
        }
    }

    RegistryKeyChange(const RegistryKeyChange&) = delete;
    RegistryKeyChange(RegistryKeyChange&& other) = default;
    const RegistryKeyChange& operator=(RegistryKeyChange&& other)
    {
        m_key = std::move(other.m_key);
        m_value = std::move(other.m_value);
    }

    const RegistryKeyChange& operator=(RegistryKeyChange&) = delete;

    void Set(const T& Value)
    {
        if constexpr (std::is_same_v<std::remove_reference_t<T>, DWORD>)
        {
            wsl::windows::common::registry::WriteDword(m_key.get(), nullptr, m_value.c_str(), Value);
        }
        else if constexpr (std::is_same_v<std::remove_reference_t<T>, std::wstring>)
        {
            wsl::windows::common::registry::WriteString(m_key.get(), nullptr, m_value.c_str(), Value.c_str());
        }
        else
        {
            static_assert(sizeof(T) != sizeof(T));
        }
    }

    auto Get() const
    {
        if constexpr (std::is_same_v<T, DWORD>)
        {
            DWORD Value = 0;
            DWORD Size = sizeof(Value);
            const auto Result = RegGetValueW(m_key.get(), nullptr, m_value.c_str(), RRF_RT_REG_DWORD, nullptr, &Value, &Size);
            if (Result == ERROR_SUCCESS)
            {
                WI_ASSERT(Size == sizeof(Value));
                return std::optional<DWORD>{Value};
            }
            else if ((Result == ERROR_PATH_NOT_FOUND) || (Result == ERROR_FILE_NOT_FOUND))
            {
                return std::optional<DWORD>{};
            }
            else
            {
                THROW_NTSTATUS(Result);
            }
        }
        else if constexpr (std::is_same_v<std::remove_reference_t<T>, std::wstring>)
        {
            return wsl::windows::common::registry::ReadOptionalString(m_key.get(), nullptr, m_value.c_str());
        }
        else
        {
            static_assert(sizeof(T) != sizeof(T));
        }
    }

private:
    wil::unique_hkey m_key;
    std::wstring m_value;
    std::optional<T> m_originalValue;
};

class ScopedEnvVariable
{
public:
    ScopedEnvVariable(const std::wstring& Name, const std::wstring& Value);
    ~ScopedEnvVariable();

    ScopedEnvVariable(const WslConfigChange&) = delete;
    ScopedEnvVariable(WslConfigChange&&) = delete;
    const ScopedEnvVariable& operator=(ScopedEnvVariable&&) = delete;
    const ScopedEnvVariable& operator=(ScopedEnvVariable&) = delete;

private:
    std::wstring m_name;
};

class UniqueWebServer
{
public:
    UniqueWebServer(LPCWSTR Endpoint, LPCWSTR ResponseContent);
    UniqueWebServer(LPCWSTR Endpoint, const std::filesystem::path& path);
    ~UniqueWebServer();
    UniqueWebServer(const UniqueWebServer&) = delete;
    UniqueWebServer(UniqueWebServer&&) = delete;

    UniqueWebServer& operator=(const UniqueWebServer&) = delete;
    UniqueWebServer& operator=(UniqueWebServer&&) = delete;

private:
    wil::unique_handle m_process;
};

class DistroFileChange
{
public:
    DistroFileChange(LPCWSTR Path, bool exists = true);
    ~DistroFileChange();
    DistroFileChange(const DistroFileChange&) = delete;
    DistroFileChange(DistroFileChange&&) = delete;
    DistroFileChange& operator=(const DistroFileChange&) = delete;
    DistroFileChange& operator=(DistroFileChange&&) = delete;

    void SetContent(LPCWSTR Content);
    void Delete();

private:
    std::optional<std::wstring> m_originalContent;
    LPCWSTR m_path{};
};

//
// Structs and enums.
//

typedef struct _LXSS_TEST_LAUNCHER_TEST
{
    ULONG NumberOfErrors;
    ULONG NumberOfPasses;
} LXSS_TEST_LAUNCHER_TEST, *PLXSS_TEST_LAUNCHER_TEST;

typedef enum LXSS_TEST_LAUNCHER_MESSAGE_TYPE
{
    LogInfoMessage,
    LogErrorMessage,
    LogPassMessage
} LXSS_TEST_LAUNCHER_MESSAGE_TYPE,
    *PLXSS_TEST_LAUNCHER_MESSAGE_TYPE;

// from nttpapi.h - need to find a way to include later
typedef LARGE_INTEGER TP_TIMESTAMP, *PTP_TIMESTAMP;

std::pair<wil::unique_handle, wil::unique_handle> CreateSubprocessPipe(
    bool inheritRead, bool inheritWrite, DWORD bufferSize = 0, _In_opt_ SECURITY_ATTRIBUTES* sa = nullptr);

std::pair<DWORD, DWORD> GetServiceState(SC_HANDLE service);

DWORD
LxsstuLaunchWsl(
    _In_opt_ LPCWSTR Arguments,
    _In_opt_ HANDLE StandardInput = nullptr,
    _In_opt_ HANDLE StandardOutput = nullptr,
    _In_opt_ HANDLE StandardError = nullptr,
    _In_opt_ HANDLE Token = nullptr,
    _In_ DWORD Flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT);

DWORD
LxsstuLaunchWsl(
    _In_opt_ const std::wstring& Arguments,
    _In_opt_ HANDLE StandardInput = nullptr,
    _In_opt_ HANDLE StandardOutput = nullptr,
    _In_opt_ HANDLE StandardError = nullptr,
    _In_opt_ HANDLE Token = nullptr);

std::pair<std::wstring, std::wstring> LxsstuLaunchWslAndCaptureOutput(
    _In_ LPCWSTR Cmd,
    _In_ int ExpectedExitCode = 0,
    _In_opt_ HANDLE StandardInput = nullptr,
    _In_opt_ HANDLE Token = nullptr,
    _In_ DWORD Flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
    _In_ LPCWSTR Entrypoint = WSL_BINARY_NAME);

std::wstring LxssGenerateWslCommandLine(_In_opt_ LPCWSTR Arguments, _In_ LPCWSTR EntryPoint = WSL_BINARY_NAME);

std::pair<std::wstring, std::wstring> LxsstuLaunchWslAndCaptureOutput(
    _In_ const std::wstring& Cmd,
    _In_ int ExpectedExitCode = 0,
    _In_opt_ HANDLE StandardInput = nullptr,
    _In_opt_ HANDLE Token = nullptr,
    _In_ DWORD Flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
    _In_ LPCWSTR EntryPoint = WSL_BINARY_NAME);

std::pair<std::wstring, std::wstring> LxsstuLaunchCommandAndCaptureOutput(
    _In_ LPWSTR Cmd, _In_ LPCSTR StandardInput, _In_opt_ HANDLE Token = nullptr, _In_ DWORD Flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT);

std::tuple<std::wstring, std::wstring, int> LxsstuLaunchCommandAndCaptureOutputWithResult(
    _In_ LPWSTR Cmd,
    _In_opt_ HANDLE StandardInput = nullptr,
    _In_opt_ HANDLE Token = nullptr,
    _In_ DWORD Flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT);

std::pair<std::wstring, std::wstring> LxsstuLaunchCommandAndCaptureOutput(
    _In_ LPWSTR Cmd,
    _In_ int ExpectedExitCode = 0,
    _In_opt_ HANDLE StandardInput = nullptr,
    _In_opt_ HANDLE Token = nullptr,
    _In_ DWORD Flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT);

DWORD
LxsstuRunCommand(
    _In_ LPWSTR Command,
    _In_opt_ HANDLE StandardInput = nullptr,
    _In_opt_ HANDLE StandardOutput = nullptr,
    _In_opt_ HANDLE StandardError = nullptr,
    _In_opt_ HANDLE Token = nullptr,
    _In_ DWORD Flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT);

wil::unique_handle LxsstuStartProcess(
    _In_ LPWSTR Command,
    _In_opt_ HANDLE StandardInput = nullptr,
    _In_opt_ HANDLE StandardOutput = nullptr,
    _In_opt_ HANDLE StandardError = nullptr,
    _In_opt_ HANDLE Token = nullptr,
    _In_ DWORD Flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT);

wil::unique_file FileFromHandle(_Inout_ wil::unique_handle& Handle, _In_ const char* Mode);

BOOL LxsstuInitialize(__in BOOLEAN RunInstanceTests);

BOOL LxsstuVmMode(VOID);

std::pair<std::wstring, std::wstring> LxsstuLaunchPowershellAndCaptureOutput(_In_ const std::wstring& Cmd, _In_ int ExpectedExitCode = 0);

VOID LxsstuUninitialize(__in BOOLEAN RunInstanceTests);

void LxssLogKernelOutput();

std::wstring LxsstuGetTestDirectory(VOID);

std::wstring LxsstuGetLxssDirectory(VOID);

VOID LxsstuInstanceTests(VOID);

VOID __stdcall LxsstuWatchdogTimer(_Inout_ PTP_CALLBACK_INSTANCE Instance, _Inout_opt_ PVOID ThreadpoolTimerContext, _Inout_ PTP_TIMER Timer);

std::vector<std::wstring> LxssSplitString(_In_ const std::wstring& string, _In_ const std::wstring& delim = L" ");

void RestartWslService();

wil::unique_handle GetNonElevatedToken();

std::wstring LxssWriteWslConfig(const std::wstring& Content);

std::string LxssWriteWslDistroConfig(const std::string& Content);

enum class DrvFsMode
{
    WSL1,
    Plan9,
    Virtio9p,
    VirtioFs
};

struct TestConfigDefaults
{
    std::optional<size_t> vmIdleTimeout;
    std::optional<bool> safeMode;
    std::optional<bool> guiApplications;
    std::optional<DrvFsMode> drvFsMode;
    std::optional<wsl::core::NetworkingMode> networkingMode;
    const std::optional<std::wstring> vmSwitch;
    const std::optional<std::wstring> macAddress;
    bool ipv6 = false;
    std::optional<bool> dnsTunneling;
    std::optional<std::wstring> dnsTunnelingIpAddress;
    std::optional<bool> dnsProxy;
    std::optional<bool> firewall;
    std::optional<bool> autoProxy;
    std::optional<std::wstring> kernel;
    std::optional<std::wstring> kernelCommandLine;
    std::optional<std::wstring> kernelModules;
    std::optional<std::wstring> loadKernelModules;
    std::optional<bool> loadDefaultKernelModules;
    std::optional<bool> sparse;
    std::optional<bool> hostAddressLoopback;
    int crashDumpCount = 100;
    std::optional<std::wstring> CrashDumpFolder;
};

std::wstring LxssGenerateTestConfig(TestConfigDefaults Default = {});

NTSTATUS
LxsstuParseLinuxLogFiles(__in PCWSTR LogFileName, __out PBOOL TestPassed);

VOID LxsstuRunTest(_In_ PCWSTR CommandLine, _In_opt_ PCWSTR LogFileName = NULL, _In_opt_ PCWSTR Username = nullptr) noexcept(false);

NTSTATUS
LxsstuParseLogFile(__in HANDLE FileHandle, __in PLXSS_TEST_LAUNCHER_TEST TestRecord);

bool ModuleSetup();

bool ModuleCleanup();

HANDLE
LxssRedirectOutput(_In_ DWORD stream, _In_ const std::wstring& file);

std::pair<HANDLE, HANDLE> UseOriginalStdHandles();

void RestoreTestStdHandles(_In_ const std::pair<HANDLE, HANDLE>& Handles);

void CreateUser(_In_ const std::wstring& Username, _Out_ PULONG Uid, _Out_ PULONG Gid);

bool TryLoadDnsResolverMethods() noexcept;

bool AreExperimentalNetworkingFeaturesSupported();

bool IsHyperVFirewallSupported() noexcept;

bool WslShutdown();

void TerminateDistribution(LPCWSTR DistributionName = LXSS_DISTRO_NAME_TEST_L);

void Trim(std::wstring& string);

inline auto EnableSystemd(const std::string& extraConfig = "")
{
    // enable systemd on the test distro by editing /etc/wsl.conf
    LxssWriteWslDistroConfig("[boot]\nsystemd=true\n" + extraConfig);
    TerminateDistribution();

    return wil::scope_exit([] {
        // clean up wsl.conf file
        LxsstuLaunchWsl(LXSST_REMOVE_DISTRO_CONF_COMMAND_LINE);
        TerminateDistribution();
    });
}

std::wstring EscapePath(std::wstring_view Path);

void StopWslService();

std::optional<GUID> GetDistributionId(LPCWSTR Name);
wil::unique_hkey OpenDistributionKey(LPCWSTR Name);

void ValidateOutput(LPCWSTR CommandLine, const std::wstring& ExpectedOutput, const std::wstring& ExpectedWarnings = L"", int ExitCode = -1);