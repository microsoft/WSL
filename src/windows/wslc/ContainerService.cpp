#include "ContainerService.h"
#include "ConsoleService.h"
#include "Utils.h"
#include <wslutil.h>
#include <WSLAProcessLauncher.h>
#include <docker_schema.h>
#include <CommandLine.h>

namespace wslc::services
{
using wsl::windows::common::wslutil::WSLAErrorDetails;
using wsl::windows::common::wslutil::PrintMessage;
using wsl::windows::common::ClientRunningWSLAProcess;
using wsl::windows::common::docker_schema::InspectContainer;
using namespace wslc::models;

static void SetContainerTTYOptions(WSLA_CONTAINER_OPTIONS& options)
{
    if (WI_IsFlagSet(options.InitProcessOptions.Flags, WSLAProcessFlagsTty))
    {
        HANDLE Stdout = GetStdHandle(STD_OUTPUT_HANDLE);
        HANDLE Stdin = GetStdHandle(STD_INPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFOEX info{};
        info.cbSize = sizeof(info);
        THROW_IF_WIN32_BOOL_FALSE(::GetConsoleScreenBufferInfoEx(Stdout, &info));
        options.InitProcessOptions.TtyColumns = info.srWindow.Right - info.srWindow.Left + 1;
        options.InitProcessOptions.TtyRows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
}

static void SetContainerArguments(WSLA_CONTAINER_OPTIONS& options, const std::vector<std::string>& args, std::vector<const char*>& argsStorage)
{
    argsStorage.clear();
    argsStorage.reserve(args.size());
    for (const auto& arg : args)
    {
        argsStorage.push_back(arg.c_str());
    }
    options.InitProcessOptions.CommandLine = {.Values = argsStorage.data(), .Count = static_cast<ULONG>(argsStorage.size())};
}

static void CreateInternal(Session& session, IWSLAContainer** container, WSLA_CONTAINER_OPTIONS& containerOptions, std::string image, const ContainerCreateOptions& options)
{
    WI_SetFlagIf(containerOptions.InitProcessOptions.Flags, WSLAProcessFlagsStdin, options.Interactive);
    WI_SetFlagIf(containerOptions.InitProcessOptions.Flags, WSLAProcessFlagsTty, options.TTY);
    containerOptions.Name = options.Name.c_str();
    containerOptions.Image = image.c_str();
    std::vector<const char*> argsStorage;
    SetContainerTTYOptions(containerOptions);
    SetContainerArguments(containerOptions, options.Arguments, argsStorage);

    WSLAErrorDetails error{};
    auto result = session.Get()->CreateContainer(&containerOptions, container, &error.Error);
    if (result == WSLA_E_IMAGE_NOT_FOUND)
    {
        PrintMessage(std::format(L"Image '{}' not found, pulling", image), stderr);
        PullImpl(*session.Get(), image);
        error.Reset();
        result = session.Get()->CreateContainer(&containerOptions, container, &error.Error);
    }

    error.ThrowIfFailed(result);
}

static void StopInternal(IWSLAContainer& container, int signal, ULONG timeout = StopContainerOptions::DefaultTimeout)
{
    THROW_IF_FAILED(container.Stop(static_cast<WSLASignal>(signal), timeout)); // TODO: Error message
}

int ContainerService::Run(Session& session, std::string image, ContainerRunOptions runOptions)
{
    // Create the container
    wil::com_ptr<IWSLAContainer> container;
    WSLA_CONTAINER_OPTIONS containerOptions{};
    CreateInternal(session, &container, containerOptions, image, runOptions);

    // Start the created container
    WSLAContainerStartFlags startFlags;
    WI_SetFlagIf(startFlags, WSLAContainerStartFlagsAttach, !runOptions.Detach);
    THROW_IF_FAILED(container->Start(startFlags)); // TODO: Error message

    // Handle attach if requested
    if (WI_IsFlagSet(startFlags, WSLAContainerStartFlagsAttach))
    {
        wil::com_ptr<IWSLAProcess> process;
        THROW_IF_FAILED(container->GetInitProcess(&process));

        ConsoleService consoleService;
        return consoleService.AttachToCurrentConsole(ClientRunningWSLAProcess(std::move(process), containerOptions.InitProcessOptions.Flags));
    }

    WSLAContainerId containerId{};
    THROW_IF_FAILED(container->GetId(containerId));
    PrintMessage(L"%hs\n", stdout, containerId);
    return 0;
}

 CreateContainerResult ContainerService::Create(Session& session, std::string image, ContainerCreateOptions runOptions)
{
    wil::com_ptr<IWSLAContainer> container;
    WSLA_CONTAINER_OPTIONS containerOptions{};
    CreateInternal(session, &container, containerOptions, image, runOptions);

    wil::unique_cotaskmem_ansistring output;
    THROW_IF_FAILED(container->Inspect(&output));

    auto inspect = wsl::shared::FromJson<InspectContainer>(output.get());
    CreateContainerResult result;
    result.Id = inspect.Id;
    return result;
}

void ContainerService::Start(Session& session, std::string id)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    THROW_IF_FAILED(container->Start(WSLAContainerStartFlags::WSLAContainerStartFlagsNone)); // TODO: Error message
}

void ContainerService::Stop(Session& session, std::string id, StopContainerOptions options)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    // AMIR
}

void ContainerService::Kill(Session& session, std::string id, int signal)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    StopInternal(*container, signal);
}

void ContainerService::Delete(Session& session, std::string id, bool force)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    if (force)
    {
        StopInternal(*container, WSLASignalSIGKILL, StopContainerOptions::DefaultTimeout);
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

int ContainerService::Exec(Session& session, std::string id, wslc::models::ExecContainerOptions options)
{
    ConsoleService consoleService;
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    int error = -1;
    WSLA_CONTAINER_OPTIONS containerOptions{};
    containerOptions.Name = id.c_str();
    containerOptions.InitProcessOptions.CurrentDirectory = nullptr;
    containerOptions.InitProcessOptions.Environment = {};
    std::vector<const char*> argsStorage;
    SetContainerTTYOptions(containerOptions);
    SetContainerArguments(containerOptions, options.Arguments, argsStorage);

    wil::com_ptr<IWSLAProcess> process;
    THROW_IF_FAILED(container->Exec(&containerOptions.InitProcessOptions, &process, &error));
    return consoleService.AttachToCurrentConsole(ClientRunningWSLAProcess(std::move(process), containerOptions.InitProcessOptions.Flags));
}

InspectContainer ContainerService::Inspect(Session& session, std::string id)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    wil::unique_cotaskmem_ansistring output;
    THROW_IF_FAILED(container->Inspect(&output));
    return wsl::shared::FromJson<InspectContainer>(output.get());
}
}
