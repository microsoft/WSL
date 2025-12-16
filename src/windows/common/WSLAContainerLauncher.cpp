/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAContainerLauncher.cpp

Abstract:

    This file contains the implementation for WSLAContainerLauncher.

--*/
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
    WSLA_CONTAINER_NETWORK_TYPE containerNetworkType,
    ProcessFlags Flags) :
    WSLAProcessLauncher(EntryPoint, Arguments, Environment, Flags), m_image(Image), m_name(Name), m_containerNetworkType(containerNetworkType)
{
}

void WSLAContainerLauncher::AddPort(uint16_t WindowsPort, uint16_t ContainerPort, int Family)
{
    m_ports.emplace_back(WSLA_PORT_MAPPING{.HostPort = WindowsPort, .ContainerPort = ContainerPort, .Family = Family});
}

std::pair<HRESULT, std::optional<RunningWSLAContainer>> WSLAContainerLauncher::LaunchNoThrow(IWSLASession& Session)
{
    WSLA_CONTAINER_OPTIONS options{};
    options.Image = m_image.c_str();
    options.Name = m_name.c_str();
    auto [processOptions, commandLinePtrs, environmentPtrs] = CreateProcessOptions();
    options.InitProcessOptions = processOptions;
    options.ContainerNetwork.ContainerNetworkType = m_containerNetworkType;
    options.Ports = m_ports.data();
    options.PortsCount = static_cast<ULONG>(m_ports.size());

    if (m_executable.empty())
    {
        options.InitProcessOptions.Executable = nullptr;
    }

    // TODO: Support volumes, ports, flags, shm size, container networking mode, etc.
    wil::com_ptr<IWSLAContainer> container;
    auto result = Session.CreateContainer(&options, &container);
    if (FAILED(result))
    {
        return std::pair<HRESULT, std::optional<RunningWSLAContainer>>(result, std::optional<RunningWSLAContainer>{});
    }

    return std::make_pair(S_OK, RunningWSLAContainer{std::move(container), std::move(m_fds)});
}

RunningWSLAContainer WSLAContainerLauncher::Launch(IWSLASession& Session)
{
    auto [result, container] = LaunchNoThrow(Session);
    THROW_IF_FAILED(result);

    return std::move(container.value());
}