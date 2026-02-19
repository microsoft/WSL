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

namespace wsl::windows::wslc::services {
using wsl::windows::common::ClientRunningWSLAProcess;
using wsl::windows::common::docker_schema::InspectContainer;
using wsl::windows::common::wslutil::PrintMessage;
using namespace wsl::windows::wslc::models;

static void SetContainerTTYOptions(WSLA_PROCESS_OPTIONS& options)
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

static void SetContainerArguments(WSLA_PROCESS_OPTIONS& options, std::vector<const char*>& argsStorage)
{
    options.CommandLine = {.Values = argsStorage.data(), .Count = static_cast<ULONG>(argsStorage.size())};
}

static wsl::windows::common::RunningWSLAContainer CreateInternal(
    Session& session, const std::string& image, const ContainerCreateOptions& options, IProgressCallback* callback)
{
    auto processFlags = WSLAProcessFlagsNone;
    WI_SetFlagIf(processFlags, WSLAProcessFlagsStdin, options.Interactive);
    WI_SetFlagIf(processFlags, WSLAProcessFlagsTty, options.TTY);
    wsl::windows::common::WSLAContainerLauncher containerLauncher(
        image, options.Name, options.Arguments, {}, WSLA_CONTAINER_NETWORK_HOST, processFlags);
    auto [result, runningContainer] = containerLauncher.CreateNoThrow(*session.Get());
    if (result == WSLA_E_IMAGE_NOT_FOUND)
    {
        PrintMessage(L"Image '%hs' not found, pulling", stderr, image.c_str());
        ImageService imageService;
        imageService.Pull(session, image, callback);
        auto [retryResult, _] = containerLauncher.CreateNoThrow(*session.Get());
        result = retryResult;
    }

    THROW_IF_FAILED(result);
    ASSERT(runningContainer);
    return std::move(*runningContainer);
}

static void StopInternal(IWSLAContainer& container, int signal = WSLASignalNone, ULONG timeout = -1)
{
    THROW_IF_FAILED(container.Stop(static_cast<WSLASignal>(signal), timeout)); // TODO: Error message
}

std::wstring ContainerService::ContainerStateToString(WSLA_CONTAINER_STATE state)
{
    switch (state)
    {
    case WSLA_CONTAINER_STATE::WslaContainerStateCreated:
        return L"created";
    case WSLA_CONTAINER_STATE::WslaContainerStateRunning:
        return L"running";
    case WSLA_CONTAINER_STATE::WslaContainerStateDeleted:
        return L"stopped";
    case WSLA_CONTAINER_STATE::WslaContainerStateExited:
        return L"exited";
    case WSLA_CONTAINER_STATE::WslaContainerStateInvalid:
    default:
        THROW_HR(E_UNEXPECTED);
    }
}

int ContainerService::Run(Session& session, const std::string& image, ContainerRunOptions runOptions, IProgressCallback* callback)
{
    // Create the container
    auto runningContainer = CreateInternal(session, image, runOptions, callback);
    runningContainer.SetDeleteOnClose(false);
    auto& container = runningContainer.Get();

    // Start the created container
    WSLAContainerStartFlags startFlags{};
    WI_SetFlagIf(startFlags, WSLAContainerStartFlagsAttach, !runOptions.Detach);
    THROW_IF_FAILED(container.Start(startFlags)); // TODO: Error message

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

CreateContainerResult ContainerService::Create(Session& session, const std::string& image, ContainerCreateOptions runOptions, IProgressCallback* callback)
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
    THROW_IF_FAILED(container->Start(WSLAContainerStartFlags::WSLAContainerStartFlagsNone)); // TODO: Error message
}

void ContainerService::Stop(Session& session, const std::string& id, StopContainerOptions options)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    StopInternal(*container, options.Signal, options.Timeout);
}

void ContainerService::Kill(Session& session, const std::string& id, int signal)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    StopInternal(*container, signal);
}

void ContainerService::Delete(Session& session, const std::string& id, bool force)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    if (force)
    {
        StopInternal(*container, WSLASignalSIGKILL);
    }

    THROW_IF_FAILED(container->Delete());
}

std::vector<ContainerInformation> ContainerService::List(Session& session)
{
    std::vector<ContainerInformation> result;
    wil::unique_cotaskmem_array_ptr<WSLA_CONTAINER> containers;
    ULONG count = 0;
    THROW_IF_FAILED(session.Get()->ListContainers(&containers, &count));
    for (auto ptr = containers.get(), end = containers.get() + count; ptr != end; ++ptr)
    {
        const WSLA_CONTAINER& current = *ptr;

        wil::com_ptr<IWSLAContainer> container;
        THROW_IF_FAILED(session.Get()->OpenContainer(current.Name, &container));

        wil::unique_cotaskmem_ansistring output;
        THROW_IF_FAILED(container->Inspect(&output));
        auto inspect = wsl::shared::FromJson<InspectContainer>(output.get());

        ContainerInformation entry;
        entry.Name = current.Name;
        entry.Image = current.Image;
        entry.State = current.State;
        entry.Id = inspect.Id;
        result.push_back(entry);
    }

    return result;
}

int ContainerService::Exec(Session& session, const std::string& id, ExecContainerOptions options)
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
} // namespace wsl::windows::wslc::services
