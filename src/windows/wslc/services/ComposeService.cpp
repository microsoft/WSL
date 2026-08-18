/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeService.cpp

Abstract:

    Implements minimal compose CLI operations.

--*/

#include "precomp.h"
#include "ComposeService.h"
#include "ConsoleService.h"
#include <relay.hpp>
#include <WSLCProcessLauncher.h>

namespace wsl::windows::wslc::services {

namespace {

    struct AttachedContainer
    {
        wsl::windows::common::ClientRunningWSLCProcess Process;
        wil::unique_handle Stdin;
        wil::unique_handle Stdout;
        wil::unique_handle Stderr;
    };

    bool IsValidHandle(HANDLE Handle)
    {
        if (Handle == nullptr || Handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        SetLastError(ERROR_SUCCESS);
        return GetFileType(Handle) != FILE_TYPE_UNKNOWN || GetLastError() == ERROR_SUCCESS;
    }

    bool IsUsableInputHandle(HANDLE Handle)
    {
        if (!IsValidHandle(Handle))
        {
            return false;
        }

        if (GetFileType(Handle) == FILE_TYPE_CHAR)
        {
            DWORD mode{};
            return GetConsoleMode(Handle, &mode);
        }

        return true;
    }

} // namespace

wil::com_ptr<IWSLCComposeSession> ComposeService::Open(models::Session& Session, const std::wstring& Path)
{
    wil::com_ptr<IWSLCComposeSession> composeSession;
    THROW_IF_FAILED(Session.Get()->CreateComposeSession(Path.c_str(), &composeSession));
    return composeSession;
}

void ComposeService::Create(models::Session& Session, const std::wstring& Path)
{
    Open(Session, Path);
}

int ComposeService::Up(Terminal& Terminal, models::Session& Session, const std::wstring& Path)
{
    Start(Session, Path);
    return Attach(Terminal, Session, Path);
}

void ComposeService::Start(models::Session& Session, const std::wstring& Path)
{
    THROW_IF_FAILED(Open(Session, Path)->Start());
}

int ComposeService::Attach(Terminal& Terminal, models::Session& Session, const std::wstring& Path)
{
    [[maybe_unused]] auto operation = Session.BeginContainerOperation();
    auto composeSession = Open(Session, Path);
    THROW_IF_FAILED(composeSession->Attach());

    wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
    THROW_IF_FAILED(composeSession->ListContainers(&containers, containers.size_address<ULONG>()));

    std::vector<AttachedContainer> attached;
    attached.reserve(containers.size());
    for (const auto& entry : containers)
    {
        wil::com_ptr<IWSLCContainer> container;
        THROW_IF_FAILED(Session.Get()->OpenContainer(entry.Id, &container));

        wil::com_ptr<IWSLCProcess> process;
        THROW_IF_FAILED(container->GetInitProcess(&process));

        WSLCProcessFlags flags{};
        THROW_IF_FAILED(process->GetFlags(&flags));

        wsl::windows::common::wslutil::COMOutputHandle stdinHandle;
        wsl::windows::common::wslutil::COMOutputHandle stdoutHandle;
        wsl::windows::common::wslutil::COMOutputHandle stderrHandle;
        THROW_IF_FAILED(container->Attach(nullptr, &stdinHandle, &stdoutHandle, &stderrHandle));

        AttachedContainer current{.Process = wsl::windows::common::ClientRunningWSLCProcess(std::move(process), flags)};
        current.Stdin = stdinHandle.Release();
        current.Stdout = stdoutHandle.Release();
        current.Stderr = stderrHandle.Release();
        attached.emplace_back(std::move(current));
    }

    THROW_HR_IF(E_UNEXPECTED, attached.empty());

    auto outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    auto errorHandle = GetStdHandle(STD_ERROR_HANDLE);
    wil::unique_hfile nullOutput;
    if (!IsValidHandle(outputHandle))
    {
        outputHandle = IsValidHandle(errorHandle) ? errorHandle : nullptr;
    }

    if (outputHandle == nullptr)
    {
        nullOutput.reset(CreateFileW(
            L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        THROW_LAST_ERROR_IF(!nullOutput);
        outputHandle = nullOutput.get();
    }

    if (!IsValidHandle(errorHandle))
    {
        errorHandle = outputHandle;
    }

    wil::unique_event secondaryRelayExit{wil::EventOptions::ManualReset};
    std::vector<std::thread> secondaryRelays;
    auto stopSecondaryRelays = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        secondaryRelayExit.SetEvent();
        for (auto& thread : secondaryRelays)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
    });

    for (size_t index = 1; index < attached.size(); ++index)
    {
        auto& current = attached[index];
        if (current.Stdout)
        {
            secondaryRelays.emplace_back(
                wsl::windows::common::relay::CreateThread(std::move(current.Stdout), outputHandle, secondaryRelayExit.get()));
            if (current.Stderr)
            {
                secondaryRelays.emplace_back(
                    wsl::windows::common::relay::CreateThread(std::move(current.Stderr), errorHandle, secondaryRelayExit.get()));
            }
        }
        else if (current.Stdin)
        {
            secondaryRelays.emplace_back(
                wsl::windows::common::relay::CreateThread(std::move(current.Stdin), outputHandle, secondaryRelayExit.get()));
        }
    }

    auto& primary = attached.front();
    const auto consoleInput = GetStdHandle(STD_INPUT_HANDLE);
    const bool hasConsoleInput = IsUsableInputHandle(consoleInput);
    if (primary.Stdout)
    {
        if (!hasConsoleInput)
        {
            primary.Stdin.reset();
        }

        ConsoleService::RelayNonTtyProcess(std::move(primary.Stdin), std::move(primary.Stdout), std::move(primary.Stderr), outputHandle, errorHandle);
    }
    else if (!hasConsoleInput)
    {
        wsl::windows::common::relay::InterruptableRelay(primary.Stdin.get(), outputHandle);
    }
    else
    {
        wsl::windows::common::ConsoleState console;
        if (!ConsoleService::RelayInteractiveTty(console, primary.Process, primary.Stdin.get(), true))
        {
            Terminal.Info(L"[detached]\n");
            return 0;
        }
    }

    int exitCode = 0;
    for (auto& current : attached)
    {
        const auto currentExitCode = current.Process.Wait();
        if (exitCode == 0 && currentExitCode != 0)
        {
            exitCode = currentExitCode;
        }
    }

    for (auto& thread : secondaryRelays)
    {
        thread.join();
    }
    stopSecondaryRelays.release();

    return exitCode;
}

void ComposeService::Stop(models::Session& Session, const std::wstring& Path, ULONG Timeout)
{
    THROW_IF_FAILED(Open(Session, Path)->Stop(Timeout));
}

} // namespace wsl::windows::wslc::services
