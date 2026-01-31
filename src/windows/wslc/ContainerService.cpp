#include "ContainerService.h"
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

int ContainerService::Run(IWSLASession& session, std::string image, CreateOptions runOptions)
{
    wil::com_ptr<IWSLAContainer> container;
    auto fds = CreateFds(runOptions);
    CreateInternal(session, &container, fds, image, runOptions);
    StartInternal(*container);

    wil::com_ptr<IWSLAProcess> process;
    THROW_IF_FAILED(container->GetInitProcess(&process));

    return InteractiveShell(ClientRunningWSLAProcess(std::move(process), std::move(fds)), runOptions.TTY);
}

 CreateContainerResult ContainerService::Create(IWSLASession& session, std::string image, CreateOptions runOptions)
{
    wil::com_ptr<IWSLAContainer> container;
    auto fds = CreateFds(runOptions);
    CreateInternal(session, &container, fds, image, runOptions);

    wil::unique_cotaskmem_ansistring output;
    THROW_IF_FAILED(container->Inspect(&output));

    auto inspect = wsl::shared::FromJson<InspectContainer>(output.get());
    CreateContainerResult result;
    result.Id = inspect.Id;
    return result;
}

void ContainerService::Start(IWSLASession& session, std::string id)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.OpenContainer(id.c_str(), &container));
    StartInternal(*container);
}

void ContainerService::Stop(IWSLASession& session, std::string id, StopContainerOptions options)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.OpenContainer(id.c_str(), &container));
    StopInternal(*container, options);
}

void ContainerService::Kill(IWSLASession& session, std::string id, int signal)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.OpenContainer(id.c_str(), &container));
    StopContainerOptions options;
    options.Signal = signal;
    StopInternal(*container, options);
}

void ContainerService::Delete(IWSLASession& session, std::string id, bool force)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.OpenContainer(id.c_str(), &container));
    if (force)
    {
        StopContainerOptions options;
        options.Signal = WSLASignalSIGKILL;
        StopInternal(*container, options);
    }

    THROW_IF_FAILED(container->Delete());
}

std::vector<ContainerInformation> ContainerService::List(IWSLASession& session)
{
    std::vector<ContainerInformation> result;
    wil::unique_cotaskmem_array_ptr<WSLA_CONTAINER> containers;
    ULONG count = 0;
    THROW_IF_FAILED(session.ListContainers(&containers, &count));
    for (auto ptr = containers.get(), end = containers.get() + count; ptr != end; ++ptr)
    {
        const WSLA_CONTAINER& current = *ptr;

        wil::com_ptr<IWSLAContainer> container;
        THROW_IF_FAILED(session.OpenContainer(current.Name, &container));

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

void ContainerService::Exec(IWSLASession& session, std::string id, std::vector<std::string> arguments)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.OpenContainer(id.c_str(), &container));
    int error = -1;
    WSLA_CONTAINER_OPTIONS options;

    // TODO tty, interactive
    auto fds = CreateFds({});
    std::vector<const char*> args;
    SetContainerOptions(options, id, false, false, fds, arguments, args);
    options.InitProcessOptions.Executable = nullptr;
    options.InitProcessOptions.CurrentDirectory = nullptr;
    options.InitProcessOptions.Environment = nullptr;
    options.InitProcessOptions.EnvironmentCount = 0;

    wil::com_ptr<IWSLAProcess> createdProcess;
    THROW_IF_FAILED(container->Exec(&options.InitProcessOptions, &createdProcess, &error));
    InteractiveShell(ClientRunningWSLAProcess(std::move(createdProcess), std::move(fds)), false);
}

InspectContainer ContainerService::Inspect(IWSLASession& session, std::string id)
{
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(session.OpenContainer(id.c_str(), &container));
    wil::unique_cotaskmem_ansistring output;
    THROW_IF_FAILED(container->Inspect(&output));
    return wsl::shared::FromJson<InspectContainer>(output.get());
}

void ContainerService::CreateInternal(
    IWSLASession& session,
    IWSLAContainer** container,
    std::vector<WSLA_PROCESS_FD>& fds,
    std::string image,
    const CreateOptions& options)
{
    WSLA_CONTAINER_OPTIONS containerOptions{};
    containerOptions.Image = image.c_str();
    std::vector<const char*> args;
    SetContainerOptions(containerOptions, options.Name, options.TTY, options.Interactive, fds, options.Arguments, args);

    WSLAErrorDetails error{};
    auto result = session.CreateContainer(&containerOptions, container, &error.Error);
    if (result == WSLA_E_IMAGE_NOT_FOUND)
    {
        PrintMessage(std::format(L"Image '{}' not found, pulling", image), stderr);

        PullImpl(session, image);

        error.Reset();
        result = session.CreateContainer(&containerOptions, container, &error.Error);
    }

    error.ThrowIfFailed(result);
}

std::vector<WSLA_PROCESS_FD> ContainerService::CreateFds(const CreateOptions& options)
{
    std::vector<WSLA_PROCESS_FD> fds;

    if (options.TTY)
    {
        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeTerminalInput});
        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeTerminalOutput});
        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeTerminalControl});
    }
    else
    {
        if (options.Interactive)
        {
            fds.emplace_back(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeDefault});
        }

        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeDefault});
        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeDefault});
    }

    return fds;
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

