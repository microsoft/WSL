#include "ContainerService.h"
#include "ConsoleService.h"
#include "Utils.h"
#include <wslutil.h>
#include <WSLAProcessLauncher.h>
#include <docker_schema.h>

namespace wslc::services
{
using wsl::windows::common::wslutil::WSLAErrorDetails;
using wsl::windows::common::wslutil::PrintMessage;
using wsl::windows::common::ClientRunningWSLAProcess;
using wsl::windows::common::docker_schema::InspectContainer;
using namespace wslc::models;

int ContainerService::Run(Session& session, std::string image, ContainerCreateOptions runOptions)
{
    ConsoleService consoleService;
    wil::com_ptr<IWSLAContainer> container;
    auto fds = consoleService.BuildStdioDescriptors(runOptions.TTY, runOptions.Interactive);
    CreateInternal(session, &container, fds, image, runOptions);
    StartInternal(*container);

    wil::com_ptr<IWSLAProcess> process;
    THROW_IF_FAILED(container->GetInitProcess(&process));

    wslc::models::ConsoleAttachOptions options;
    options.TTY = runOptions.TTY;
    options.Interactive = runOptions.Interactive;
    return consoleService.AttachToCurrentConsole(std::move(process), options);
}

 CreateContainerResult ContainerService::Create(Session& session, std::string image, ContainerCreateOptions runOptions)
{
    ConsoleService consoleService;
    wil::com_ptr<IWSLAContainer> container;
    auto fds = consoleService.BuildStdioDescriptors(runOptions.TTY, runOptions.Interactive);
    CreateInternal(session, &container, fds, image, runOptions);

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
    StartInternal(*container);
}

void ContainerService::Stop(Session& session, std::string id, StopContainerOptions options)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    StopInternal(*container, options);
}

void ContainerService::Kill(Session& session, std::string id, int signal)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    StopContainerOptions options;
    options.Signal = signal;
    StopInternal(*container, options);
}

void ContainerService::Delete(Session& session, std::string id, bool force)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    if (force)
    {
        StopContainerOptions options;
        options.Signal = WSLASignalSIGKILL;
        StopInternal(*container, options);
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
    WSLA_CONTAINER_OPTIONS containerOptions;

    auto fds = consoleService.BuildStdioDescriptors(options.TTY, options.Interactive);
    std::vector<const char*> args;
    SetContainerOptions(containerOptions, id, false, false, fds, options.Arguments, args);
    containerOptions.InitProcessOptions.Executable = nullptr;
    containerOptions.InitProcessOptions.CurrentDirectory = nullptr;
    containerOptions.InitProcessOptions.Environment = nullptr;
    containerOptions.InitProcessOptions.EnvironmentCount = 0;

    wil::com_ptr<IWSLAProcess> process;
    THROW_IF_FAILED(container->Exec(&containerOptions.InitProcessOptions, &process, &error));
    wslc::models::ConsoleAttachOptions attachOptions;
    attachOptions.TTY = options.TTY;
    attachOptions.Interactive = options.Interactive;
    return consoleService.AttachToCurrentConsole(std::move(process), attachOptions);
}

InspectContainer ContainerService::Inspect(Session& session, std::string id)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    wil::unique_cotaskmem_ansistring output;
    THROW_IF_FAILED(container->Inspect(&output));
    return wsl::shared::FromJson<InspectContainer>(output.get());
}

void ContainerService::CreateInternal(
    Session& session,
    IWSLAContainer** container,
    std::vector<WSLA_PROCESS_FD>& fds,
    std::string image,
    const ContainerCreateOptions& options)
{
    WSLA_CONTAINER_OPTIONS containerOptions{};
    containerOptions.Image = image.c_str();
    std::vector<const char*> args;
    SetContainerOptions(containerOptions, options.Name, options.TTY, options.Interactive, fds, options.Arguments, args);

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

void ContainerService::StartInternal(IWSLAContainer& container)
{
    THROW_IF_FAILED(container.Start()); // TODO: Error message
}

void ContainerService::StopInternal(IWSLAContainer& container, const StopContainerOptions& options)
{
    THROW_IF_FAILED(container.Stop(options.Signal, options.Timeout)); // TODO: Error message
}

void ContainerService::SetContainerOptions(
    WSLA_CONTAINER_OPTIONS& options,
    const std::string& name,
    bool tty,
    bool interactive,
    std::vector<WSLA_PROCESS_FD>& fds,
    const std::vector<std::string>& arguments,
    std::vector<const char*>& args)
{
    HANDLE Stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE Stdin = GetStdHandle(STD_INPUT_HANDLE);
    if (tty)
    {
        CONSOLE_SCREEN_BUFFER_INFOEX Info{};
        Info.cbSize = sizeof(Info);
        THROW_IF_WIN32_BOOL_FALSE(::GetConsoleScreenBufferInfoEx(Stdout, &Info));
        options.InitProcessOptions.TtyColumns = Info.srWindow.Right - Info.srWindow.Left + 1;
        options.InitProcessOptions.TtyRows = Info.srWindow.Bottom - Info.srWindow.Top + 1;
    }

    args.clear();
    args.reserve(arguments.size());
    for (const auto& arg : arguments)
    {
        args.push_back(arg.c_str());
    }

    options.Name = name.c_str();
    options.InitProcessOptions.CommandLine = args.data();
    options.InitProcessOptions.CommandLineCount = static_cast<ULONG>(args.size());
    options.InitProcessOptions.Fds = fds.data();
    options.InitProcessOptions.FdsCount = static_cast<ULONG>(fds.size());
}
}

