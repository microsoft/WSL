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

void wsl::windows::common::WSLAContainerLauncher::AddVolume(const std::wstring& HostPath, const std::string& ContainerPath, bool ReadOnly)
{
    // Store a copy of the path strings to the launcher to ensure the pointers in WSLA_VOLUME remain valid.
    const auto& hostPath = m_hostPaths.emplace_back(HostPath);
    const auto& containerPath = m_containerPaths.emplace_back(ContainerPath);

    WSLA_VOLUME vol{};
    vol.HostPath = hostPath.c_str();
    vol.ContainerPath = containerPath.c_str();
    vol.ReadOnly = ReadOnly ? TRUE : FALSE;

    m_volumes.push_back(vol);
}

std::pair<HRESULT, std::optional<RunningWSLAContainer>> WSLAContainerLauncher::LaunchNoThrow(IWSLASession& Session)
{
    WSLA_CONTAINER_OPTIONS options{};
    options.Image = m_image.c_str();
    options.Name = m_name.c_str();
    auto [processOptions, commandLinePtrs, environmentPtrs] = CreateProcessOptions();
    options.InitProcessOptions = processOptions;
    options.ContainerNetwork.ContainerNetworkType = m_containerNetworkType;

    if (m_executable.empty())
    {
        options.InitProcessOptions.Executable = nullptr;
    }

    options.VolumesCount = static_cast<ULONG>(m_volumes.size());
    options.Volumes = m_volumes.size() > 0 ? m_volumes.data() : nullptr;

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