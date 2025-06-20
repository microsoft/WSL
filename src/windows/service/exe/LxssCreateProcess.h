/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssCreateProcess.h

Abstract:

    This file contains process creation function declarations.

--*/

#pragma once

#include "SocketChannel.h"
#include "WslPluginApi.h"

// Macro to test if Windows interop is enabled.
#define LXSS_INTEROP_FLAGS (LXSS_DISTRO_FLAGS_ENABLE_DRIVE_MOUNTING | LXSS_DISTRO_FLAGS_ENABLE_INTEROP)

#define LXSS_INTEROP_ENABLED(_flags) (((_flags) & LXSS_INTEROP_FLAGS) == LXSS_INTEROP_FLAGS)

using CreateLxProcessConsoleData = struct
{
    wil::unique_handle ConsoleHandle;
    wil::unique_handle ClientProcess;
};

using CreateLxProcessContext = struct
{
    ULONG Flags;
    std::vector<std::string> DefaultEnvironment;
    wil::unique_handle UserToken;
    bool Elevated;
};

using CreateLxProcessData = struct
{
    std::string Filename;
    std::vector<std::string> CommandLine;
    std::vector<std::string> Environment;
    std::vector<std::string> NtEnvironment;
    CREATE_PROCESS_SHELL_OPTIONS ShellOptions;
    std::string NtPath;
    std::string CurrentWorkingDirectory;
    std::string Username;
};

class LxssCreateProcess
{
public:
    /// <summary>
    /// Allocates and initializes a create process message.
    /// </summary>
    static std::vector<gsl::byte> CreateMessage(_In_ LX_MESSAGE_TYPE MessageType, _In_ const CreateLxProcessData& CreateProcessData, _In_ ULONG DefaultUid);

    /// <summary>
    /// Parses create process arguments.
    /// </summary>
    static CreateLxProcessData ParseArguments(
        _In_opt_ LPCSTR Filename,
        _In_ ULONG CommandLineCount,
        _In_reads_opt_(CommandLineCount) LPCSTR* CommandLine,
        _In_opt_ LPCWSTR CurrentWorkingDirectory,
        _In_opt_ LPCWSTR NtPath,
        _In_reads_opt_(NtEnvironmentLength) PWCHAR NtEnvironment,
        _In_ ULONG NtEnvironmentLength,
        _In_opt_ LPCWSTR Username,
        _In_ const std::vector<std::string>& DefaultEnvironment,
        _In_ ULONG Flags);

    static inline wil::unique_socket CreateLinuxProcess(
        _In_ LPCSTR Path, _In_ LPCSTR* Arguments, const GUID& RuntimeId, wsl::shared::SocketChannel& channel, HANDLE terminatingEvent, DWORD Timeout)
    {
        std::vector<char> ArgumentsData;
        for (const auto* e = Arguments; *e != nullptr; e++)
        {
            ArgumentsData.insert(ArgumentsData.end(), *e, *e + strlen(*e) + 1);
        }

        ArgumentsData.emplace_back('\0');

        wsl::shared::MessageWriter<CREATE_PROCESS_MESSAGE> message(LxInitCreateProcess);
        message.WriteString(message->PathIndex, Path);
        gsl::copy(as_bytes(gsl::span(ArgumentsData)), message.InsertBuffer(message->CommandLineIndex, ArgumentsData.size()));
        channel.SendMessage<CREATE_PROCESS_MESSAGE>(message.Span());

        auto readResult = [&]() {
            const auto& message = channel.ReceiveMessage<RESULT_MESSAGE<int32_t>>(nullptr, Timeout);
            return message.Result;
        };

        auto processSocket = wsl::windows::common::hvsocket::Connect(RuntimeId, readResult(), terminatingEvent);

        const auto execResult = readResult();
        THROW_HR_IF_MSG(E_FAIL, execResult != 0, "Failed to execute '%hs', error=%d", Path, execResult);

        return processSocket;
    }
};

typedef struct _LXSS_DISTRO_CONFIGURATION
{
    GUID DistroId;
    DWORD State;
    std::wstring Name;
    DWORD Version;
    std::filesystem::path BasePath;
    std::wstring PackageFamilyName;
    std::filesystem::path VhdFilePath;
    ULONG Flags;
    std::wstring Flavor;
    std::wstring OsVersion;
    std::optional<std::wstring> ShortcutPath;
    bool RunOOBE;
} LXSS_DISTRO_CONFIGURATION, *PLXSS_DISTRO_CONFIGURATION;

class LxssRunningInstance
{
public:
    LxssRunningInstance(int IdleTimeout) : m_idleTimeout(IdleTimeout)
    {
    }

    LxssRunningInstance(const LxssRunningInstance&) = delete;
    LxssRunningInstance(LxssRunningInstance&&) = delete;
    void operator=(const LxssRunningInstance&) = delete;
    void operator=(LxssRunningInstance&&) = delete;

    virtual void CreateLxProcess(
        _In_ const CreateLxProcessData& CreateProcessData,
        _In_ const CreateLxProcessContext& CreateProcessContext,
        _In_ const CreateLxProcessConsoleData& ConsoleData,
        _In_ SHORT Columns,
        _In_ SHORT Rows,
        _In_ PLXSS_STD_HANDLES StdHandles,
        _Out_ GUID* InstanceId,
        _Out_ HANDLE* ProcessHandle,
        _Out_ HANDLE* ServerHandle,
        _Out_ HANDLE* StandardIn,
        _Out_ HANDLE* StandardOut,
        _Out_ HANDLE* StandardErr,
        _Out_ HANDLE* CommunicationChannel,
        _Out_ HANDLE* InteropSocket) = 0;

    virtual GUID GetDistributionId() const = 0;
    virtual std::shared_ptr<LxssPort> GetInitPort() = 0;
    virtual ULONG64 GetLifetimeManagerId() const = 0;
    virtual ULONG GetClientId() const = 0;
    virtual void Initialize() = 0;
    virtual bool RequestStop(_In_ bool Force) = 0;
    virtual void Stop() = 0;
    virtual void RegisterPlan9ConnectionTarget(_In_ HANDLE userToken) = 0;
    virtual void UpdateTimezone() = 0;
    virtual const WSLDistributionInformation* DistributionInformation() const noexcept = 0;

    int GetIdleTimeout() const noexcept
    {
        return m_idleTimeout;
    };

private:
    int m_idleTimeout;
};
