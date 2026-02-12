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

static std::string GetContainerName(const std::string& name)
{
    if (!name.empty())
    {
        return name;
    }

    GUID guid;
    THROW_IF_FAILED(CoCreateGuid(&guid));
    return wsl::shared::string::GuidToString<char>(guid, wsl::shared::string::GuidToStringFlags::None);
}

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

static void CreateInternal(
    Session& session,
    IWSLAContainer** container,
    WSLA_CONTAINER_OPTIONS& containerOptions,
    const std::string& image,
    const ContainerCreateOptions& options,
    IProgressCallback* callback)
{
    auto containerName = GetContainerName(options.Name);
    WI_SetFlagIf(containerOptions.InitProcessOptions.Flags, WSLAProcessFlagsStdin, options.Interactive);
    WI_SetFlagIf(containerOptions.InitProcessOptions.Flags, WSLAProcessFlagsTty, options.TTY);
    containerOptions.Name = containerName.c_str();
    containerOptions.Image = image.c_str();
    auto argsStorage = wsl::shared::string::StringPointersFromArray(options.Arguments, false);
    SetContainerTTYOptions(containerOptions.InitProcessOptions);
    SetContainerArguments(containerOptions.InitProcessOptions, argsStorage);

    auto result = session.Get()->CreateContainer(&containerOptions, container);
    if (result == WSLA_E_IMAGE_NOT_FOUND)
    {
        PrintMessage(std::format(L"Image '{}' not found, pulling", image), stderr);
        ImageService imageService;
        imageService.Pull(session, image, callback);
        result = session.Get()->CreateContainer(&containerOptions, container);
    }

    THROW_IF_FAILED(result);
}

static void StopInternal(IWSLAContainer& container, int signal = WSLASignalNone, ULONG timeout = -1)
{
    THROW_IF_FAILED(container.Stop(static_cast<WSLASignal>(signal), timeout)); // TODO: Error message
}

int ContainerService::Run(Session& session, const std::string& image, ContainerRunOptions runOptions, IProgressCallback* callback)
{
    // Create the container
    wil::com_ptr<IWSLAContainer> container;
    WSLA_CONTAINER_OPTIONS containerOptions{};
    CreateInternal(session, &container, containerOptions, image, runOptions, callback);

    // Start the created container
    WSLAContainerStartFlags startFlags{};
    WI_SetFlagIf(startFlags, WSLAContainerStartFlagsAttach, !runOptions.Detach);
    THROW_IF_FAILED(container->Start(startFlags)); // TODO: Error message

    // Handle attach if requested
    if (WI_IsFlagSet(startFlags, WSLAContainerStartFlagsAttach))
    {
        wil::com_ptr<IWSLAProcess> process;
        THROW_IF_FAILED(container->GetInitProcess(&process));

        ConsoleService consoleService;
        return consoleService.AttachToCurrentConsole(
            ClientRunningWSLAProcess(std::move(process), containerOptions.InitProcessOptions.Flags));
    }

    WSLAContainerId containerId{};
    THROW_IF_FAILED(container->GetId(containerId));
    PrintMessage(L"%hs\n", stdout, containerId);
    return 0;
}

CreateContainerResult ContainerService::Create(Session& session, const std::string& image, ContainerCreateOptions runOptions, IProgressCallback* callback)
{
    wil::com_ptr<IWSLAContainer> container;
    WSLA_CONTAINER_OPTIONS containerOptions{};
    CreateInternal(session, &container, containerOptions, image, runOptions, callback);
    WSLAContainerId id{};
    THROW_IF_FAILED(container->GetId(id));
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

    // Set up the options for the process to be created
    WSLA_PROCESS_OPTIONS processOptions{};
    WI_SetFlagIf(processOptions.Flags, WSLAProcessFlagsStdin, options.Interactive);
    WI_SetFlagIf(processOptions.Flags, WSLAProcessFlagsTty, options.TTY);
    processOptions.CurrentDirectory = nullptr;
    processOptions.Environment = {};
    auto argsStorage = wsl::shared::string::StringPointersFromArray(options.Arguments, false);
    SetContainerTTYOptions(processOptions);
    SetContainerArguments(processOptions, argsStorage);

    // Execute the process inside the container
    wil::com_ptr<IWSLAProcess> process;
    int error = -1;
    THROW_IF_FAILED(container->Exec(&processOptions, &process, &error));
    ConsoleService consoleService;
    return consoleService.AttachToCurrentConsole(ClientRunningWSLAProcess(std::move(process), processOptions.Flags));
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
