/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerService.cpp

Abstract:

    This file contains the ContainerService implementation

--*/
#include "ContainerService.h"
#include "ConsoleService.h"
#include "ImageService.h"
#include <wslutil.h>
#include <WSLAProcessLauncher.h>
#include <docker_schema.h>
#include <CommandLine.h>
#include <unordered_map>
#include <wslaservice.h>

namespace wsl::windows::wslc::services {
using wsl::windows::common::ClientRunningWSLAProcess;
using wsl::windows::common::docker_schema::InspectContainer;
using wsl::windows::common::wslutil::PrintMessage;
using namespace wsl::windows::wslc::models;
using namespace std::chrono_literals;

DEFINE_ENUM_FLAG_OPERATORS(WSLALogsFlags);

static void SetContainerTTYOptions(WSLAProcessOptions& options)
{
    if (!WI_IsFlagSet(options.Flags, WSLAProcessFlagsTty))
    {
        return;
    }

    auto tryGetConsoleInfo = [](HANDLE handle, CONSOLE_SCREEN_BUFFER_INFOEX& info) -> bool {
        info.cbSize = sizeof(info);
        return ::GetConsoleScreenBufferInfoEx(handle, &info) != FALSE;
    };

    CONSOLE_SCREEN_BUFFER_INFOEX info{};
    HANDLE stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (tryGetConsoleInfo(stdoutHandle, info))
    {
        options.TtyColumns = info.srWindow.Right - info.srWindow.Left + 1;
        options.TtyRows = info.srWindow.Bottom - info.srWindow.Top + 1;
        return;
    }

    HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD stdinMode = 0;
    if (::GetConsoleMode(stdinHandle, &stdinMode))
    {
        wil::unique_hfile consoleOutput(CreateFileW(
            L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
        if (consoleOutput && tryGetConsoleInfo(consoleOutput.get(), info))
        {
            options.TtyColumns = info.srWindow.Right - info.srWindow.Left + 1;
            options.TtyRows = info.srWindow.Bottom - info.srWindow.Top + 1;
            return;
        }
    }

    PrintMessage(L"error: --tty requires stdin or stdout to be a console", stderr);
    THROW_HR(E_FAIL);
}

static void SetContainerArguments(WSLAProcessOptions& options, std::vector<const char*>& argsStorage)
{
    options.CommandLine = {.Values = argsStorage.data(), .Count = static_cast<ULONG>(argsStorage.size())};
}

static wsl::windows::common::RunningWSLAContainer CreateInternal(
    Session& session, const std::string& image, const ContainerOptions& options, IProgressCallback* callback)
{
    auto processFlags = WSLAProcessFlagsNone;
    WI_SetFlagIf(processFlags, WSLAProcessFlagsStdin, options.Interactive);
    WI_SetFlagIf(processFlags, WSLAProcessFlagsTty, options.TTY);
    wsl::windows::common::WSLAContainerLauncher containerLauncher(
        image, options.Name, options.Arguments, {}, WSLAContainerNetworkTypeHost, processFlags);
    auto [result, runningContainer] = containerLauncher.CreateNoThrow(*session.Get());
    if (result == WSLA_E_IMAGE_NOT_FOUND)
    {
        PrintMessage(L"Image '%hs' not found, pulling", stderr, image.c_str());
        ImageService imageService;
        imageService.Pull(session, image, callback);
        return containerLauncher.Create(*session.Get());
    }

    THROW_IF_FAILED(result);
    ASSERT(runningContainer);
    return std::move(*runningContainer);
}

static void StopInternal(IWSLAContainer& container, WSLASignal signal = WSLASignalNone, LONG timeout = -1)
{
    THROW_IF_FAILED(container.Stop(signal, timeout)); // TODO: Error message
}

static std::wstring FormatRelativeTime(ULONGLONG timestamp)
{
    constexpr LONGLONG SecondsPerMinute = std::chrono::duration_cast<std::chrono::seconds>(1min).count();
    constexpr LONGLONG SecondsPerHour = std::chrono::duration_cast<std::chrono::seconds>(1h).count();
    constexpr LONGLONG SecondsPerDay = std::chrono::duration_cast<std::chrono::seconds>(24h).count();

    auto elapsed = static_cast<LONGLONG>(std::time(nullptr)) - static_cast<LONGLONG>(timestamp);
    if (elapsed < 0)
    {
        elapsed = 0;
    }

    if (elapsed < SecondsPerMinute)
    {
        return std::format(L"{} seconds ago", elapsed);
    }
    else if (elapsed < SecondsPerHour)
    {
        return std::format(L"{} minutes ago", elapsed / SecondsPerMinute);
    }
    else if (elapsed < SecondsPerDay)
    {
        return std::format(L"{} hours ago", elapsed / SecondsPerHour);
    }

    return std::format(L"{} days ago", elapsed / SecondsPerDay);
}

int ContainerService::Attach(Session& session, const std::string& id)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));

    wil::com_ptr<IWSLAProcess> process;
    THROW_IF_FAILED(container->GetInitProcess(&process));

    wsl::windows::common::ClientRunningWSLAProcess runningProcess(std::move(process), {});

    ULONG stdinLogsHandle = 0;
    ULONG stdoutLogsHandle = 0;
    ULONG stderrLogsHandle = 0;
    THROW_IF_FAILED(container->Attach(nullptr, &stdinLogsHandle, &stdoutLogsHandle, &stderrLogsHandle));

    wil::unique_handle stdinLogs(ULongToHandle(stdinLogsHandle));
    wil::unique_handle stdoutLogs(ULongToHandle(stdoutLogsHandle));
    wil::unique_handle stderrLogs(ULongToHandle(stderrLogsHandle));

    if (stdoutLogs)
    {
        // Non-TTY process - relay separate stdout/stderr streams
        WI_ASSERT(!!stderrLogs);
        ConsoleService::RelayNonTtyProcess(std::move(stdinLogs), std::move(stdoutLogs), std::move(stderrLogs));
    }
    else
    {
        // TTY process - relay using interactive TTY handling
        WI_ASSERT(!stderrLogs);
        if (!ConsoleService::RelayInteractiveTty(runningProcess, stdinLogs.get(), true))
        {
            wsl::windows::common::wslutil::PrintMessage(L"[detached]", stderr);
            return 0; // Exit early if user detached
        }
    }

    // Wait for the container process to exit
    return runningProcess.Wait();
}

std::wstring ContainerService::ContainerStateToString(WSLAContainerState state, ULONGLONG stateChangedAt)
{
    std::wstring stateString;
    switch (state)
    {
    case WSLAContainerState::WslaContainerStateCreated:
        stateString = L"created";
        break;
    case WSLAContainerState::WslaContainerStateRunning:
        stateString = L"running";
        break;
    case WSLAContainerState::WslaContainerStateDeleted:
        stateString = L"stopped";
        break;
    case WSLAContainerState::WslaContainerStateExited:
        stateString = L"exited";
        break;
    case WSLAContainerState::WslaContainerStateInvalid:
        return L"invalid";
    default:
        THROW_HR(E_UNEXPECTED);
    }

    if (stateChangedAt == 0)
    {
        return stateString;
    }

    return std::format(L"{} {}", stateString, FormatRelativeTime(stateChangedAt));
}

std::wstring ContainerService::ContainerCreatedAtToString(ULONGLONG createdAt)
{
    return FormatRelativeTime(createdAt);
}

int ContainerService::Run(Session& session, const std::string& image, ContainerOptions runOptions, IProgressCallback* callback)
{
    // Create the container
    auto runningContainer = CreateInternal(session, image, runOptions, callback);
    runningContainer.SetDeleteOnClose(false);
    auto& container = runningContainer.Get();

    // Start the created container
    WSLAContainerStartFlags startFlags{};
    WI_SetFlagIf(startFlags, WSLAContainerStartFlagsAttach, !runOptions.Detach);
    THROW_IF_FAILED(container.Start(startFlags, nullptr)); // TODO: Error message, detach keys

    // Handle attach if requested
    if (WI_IsFlagSet(startFlags, WSLAContainerStartFlagsAttach))
    {
        ConsoleService consoleService;
        return consoleService.AttachToCurrentConsole(runningContainer.GetInitProcess());
    }

    WSLAContainerId containerId{};
    THROW_IF_FAILED(container.GetId(containerId));
    PrintMessage(L"%hs", stdout, containerId);
    return 0;
}

CreateContainerResult ContainerService::Create(Session& session, const std::string& image, ContainerOptions runOptions, IProgressCallback* callback)
{
    auto runningContainer = CreateInternal(session, image, runOptions, callback);
    runningContainer.SetDeleteOnClose(false);
    auto& container = runningContainer.Get();
    WSLAContainerId id{};
    THROW_IF_FAILED(container.GetId(id));
    return {.Id = id};
}

void ContainerService::Start(Session& session, const std::string& id)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    THROW_IF_FAILED(container->Start(WSLAContainerStartFlags::WSLAContainerStartFlagsNone, nullptr)); // TODO: Error message, detach keys
}

void ContainerService::Stop(Session& session, const std::string& id, StopContainerOptions options)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    StopInternal(*container, options.Signal, options.Timeout);
}

void ContainerService::Kill(Session& session, const std::string& id, WSLASignal signal)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    StopInternal(*container, signal);
}

void ContainerService::Delete(Session& session, const std::string& id, bool force)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    THROW_IF_FAILED(container->Delete(force ? WSLADeleteFlagsForce : WSLADeleteFlagsNone));
}

std::vector<ContainerInformation> ContainerService::List(Session& session)
{
    std::vector<ContainerInformation> result;
    wil::unique_cotaskmem_array_ptr<WSLAContainerEntry> containers;
    THROW_IF_FAILED(session.Get()->ListContainers(&containers, containers.size_address<ULONG>()));
    for (const auto& current : containers)
    {
        ContainerInformation entry;
        entry.Name = current.Name;
        entry.Image = current.Image;
        entry.State = current.State;
        entry.Id = current.Id;
        entry.StateChangedAt = current.StateChangedAt;
        entry.CreatedAt = current.CreatedAt;
        result.emplace_back(std::move(entry));
    }

    return result;
}

int ContainerService::Exec(Session& session, const std::string& id, ContainerOptions options)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));

    auto execFlags = WSLAProcessFlagsNone;
    WI_SetFlagIf(execFlags, WSLAProcessFlagsStdin, options.Interactive);
    WI_SetFlagIf(execFlags, WSLAProcessFlagsTty, options.TTY);

    ConsoleService consoleService;
    return consoleService.AttachToCurrentConsole(
        wsl::windows::common::WSLAProcessLauncher({}, options.Arguments, {}, execFlags).Launch(*container));
}

InspectContainer ContainerService::Inspect(Session& session, const std::string& id)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    wil::unique_cotaskmem_ansistring output;
    THROW_IF_FAILED(container->Inspect(&output));
    return wsl::shared::FromJson<InspectContainer>(output.get());
}

void ContainerService::Logs(Session& session, const std::string& id, bool follow)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));

    ULONG stdoutLogsHandle = 0;
    ULONG stderrLogsHandle = 0;
    WSLALogsFlags flags = WSLALogsFlagsNone;
    WI_SetFlagIf(flags, WSLALogsFlagsFollow, follow);

    THROW_IF_FAILED(container->Logs(flags, &stdoutLogsHandle, &stderrLogsHandle, 0, 0, 0));
    wil::unique_handle stdoutLogs(ULongToHandle(stdoutLogsHandle));
    wil::unique_handle stderrLogs(ULongToHandle(stderrLogsHandle));

    wsl::windows::common::relay::MultiHandleWait io;
    io.AddHandle(std::make_unique<wsl::windows::common::relay::RelayHandle<wsl::windows::common::relay::ReadHandle>>(
        std::move(stdoutLogs), GetStdHandle(STD_OUTPUT_HANDLE)));
    if (stderrLogs) // This handle is only used for non-tty processes.
    {
        io.AddHandle(std::make_unique<wsl::windows::common::relay::RelayHandle<wsl::windows::common::relay::ReadHandle>>(
            std::move(stderrLogs), GetStdHandle(STD_ERROR_HANDLE)));
    }

    // TODO: Handle ctrl-c.
    io.Run({});
}
} // namespace wsl::windows::wslc::services
