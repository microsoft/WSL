#include "WSLAContainerLauncher.h"

using wsl::windows::common::ClientRunningWSLAProcess;
using wsl::windows::common::RunningWSLAContainer;
using wsl::windows::common::WSLAContainerLauncher;

RunningWSLAContainer::RunningWSLAContainer(wil::com_ptr<IWSLAContainer>&& Container, std::vector<WSLA_PROCESS_FD>&& fds) :
    m_container(std::move(Container)), m_fds(std::move(fds))
{
}

IWSLAContainer& RunningWSLAContainer::Get()
{
    return *m_container;
}

WSLA_CONTAINER_STATE RunningWSLAContainer::State()
{
    WSLA_CONTAINER_STATE state{};
    THROW_IF_FAILED(m_container->GetState(&state));
    return state;
}

ClientRunningWSLAProcess RunningWSLAContainer::GetInitProcess()
{
    wil::com_ptr<IWSLAProcess> process;
    THROW_IF_FAILED(m_container->GetInitProcess(&process));

    return ClientRunningWSLAProcess{std::move(process), std::move(m_fds)};
}

WSLAContainerLauncher::WSLAContainerLauncher(
    const std::string& Image,
    const std::string& Name,
    const std::string& EntryPoint,
    const std::vector<std::string>& Arguments,
    const std::vector<std::string>& Environment,
    ProcessFlags Flags) :
    WSLAProcessLauncher(EntryPoint, Arguments, Environment, Flags), m_image(Image), m_name(Name)
{
}

RunningWSLAContainer WSLAContainerLauncher::Launch(IWSLASession& Session)
{
    WSLA_CONTAINER_OPTIONS options{};
    options.Image = m_image.c_str();
    options.Name = m_name.c_str();
    auto [processOptions, commandLinePtrs, environmentPtrs] = CreateProcessOptions();
    options.InitProcessOptions = processOptions;

    if (m_executable.empty())
    {
        options.InitProcessOptions.Executable = nullptr;
    }

    // TODO: Support volumes, ports, flags, shm size, etc.
    wil::com_ptr<IWSLAContainer> container;
    THROW_IF_FAILED(Session.CreateContainer(&options, &container));

    return RunningWSLAContainer{std::move(container), std::move(m_fds)};
}