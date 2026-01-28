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

RunningWSLAContainer::RunningWSLAContainer(wil::com_ptr<IWSLAContainer>&& Container, WSLAProcessFlags Flags) :
    m_container(std::move(Container)), m_flags(Flags)
{
}

RunningWSLAContainer::~RunningWSLAContainer()
{
    Reset();
}

IWSLAContainer& RunningWSLAContainer::Get()
{
    return *m_container;
}

void RunningWSLAContainer::Reset()
{
    if (m_container && m_deleteOnClose)
    {
        // Attempt to stop and delete the container.
        LOG_IF_FAILED(m_container->Stop(9, 0));
        LOG_IF_FAILED(m_container->Delete());
    }

    m_container.reset();
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

    return ClientRunningWSLAProcess{std::move(process), m_flags};
}

void RunningWSLAContainer::SetDeleteOnClose(bool deleteOnClose)
{
    m_deleteOnClose = deleteOnClose;
}

std::string RunningWSLAContainer::Id()
{
    WSLAContainerId id{};
    THROW_IF_FAILED(m_container->GetId(id));

    return id;
}

std::string RunningWSLAContainer::Name()
{
    wil::unique_cotaskmem_ansistring name;
    THROW_IF_FAILED(m_container->GetName(&name));

    return name.get();
}

WSLAContainerLauncher::WSLAContainerLauncher(
    const std::string& Image,
    const std::string& Name,
    const std::vector<std::string>& Arguments,
    const std::vector<std::string>& Environment,
    WSLA_CONTAINER_NETWORK_TYPE containerNetworkType,
    WSLAProcessFlags Flags) :
    WSLAProcessLauncher({}, Arguments, Environment, Flags), m_image(Image), m_name(Name), m_containerNetworkType(containerNetworkType)
{
}

void WSLAContainerLauncher::AddPort(uint16_t WindowsPort, uint16_t ContainerPort, int Family)
{
    m_ports.emplace_back(WSLA_PORT_MAPPING{.HostPort = WindowsPort, .ContainerPort = ContainerPort, .Family = Family});
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
    auto [result, container] = CreateNoThrow(Session);
    if (FAILED(result))
    {
        return std::make_pair(result, std::optional<RunningWSLAContainer>{});
    }

    result = container.value().Get().Start();

    return std::make_pair(result, std::move(container));
}

std::pair<HRESULT, std::optional<RunningWSLAContainer>> WSLAContainerLauncher::CreateNoThrow(IWSLASession& Session)
{
    WSLA_CONTAINER_OPTIONS options{};
    options.Image = m_image.c_str();

    if (!m_name.empty())
    {
        options.Name = m_name.c_str();
    }

    std::vector<const char*> entrypointStorage;

    for (const auto &e: m_entrypoint)
    {
        entrypointStorage.push_back(e.c_str());
    }

    auto [processOptions, commandLinePtrs, environmentPtrs] = CreateProcessOptions();
    options.InitProcessOptions = processOptions;
    options.ContainerNetwork.ContainerNetworkType = m_containerNetworkType;
    options.Ports = m_ports.data();
    options.PortsCount = static_cast<ULONG>(m_ports.size());

    if (!entrypointStorage.empty())
    {
        options.Entrypoint = {entrypointStorage.data(), static_cast<ULONG>(entrypointStorage.size())};
    }

    options.VolumesCount = static_cast<ULONG>(m_volumes.size());
    options.Volumes = m_volumes.size() > 0 ? m_volumes.data() : nullptr;

    // TODO: Support volumes, ports, flags, shm size, container networking mode, etc.
    wil::com_ptr<IWSLAContainer> container;
    auto result = Session.CreateContainer(&options, &container, nullptr);
    if (FAILED(result))
    {
        return std::pair<HRESULT, std::optional<RunningWSLAContainer>>(result, std::optional<RunningWSLAContainer>{});
    }

    return std::make_pair(S_OK, std::move(RunningWSLAContainer{std::move(container), m_flags}));
}

RunningWSLAContainer WSLAContainerLauncher::Launch(IWSLASession& Session)
{
    auto [result, container] = LaunchNoThrow(Session);
    THROW_IF_FAILED(result);

    return std::move(container.value());
}

wsl::windows::common::docker_schema::InspectContainer RunningWSLAContainer::Inspect()
{
    wil::unique_cotaskmem_ansistring output;
    THROW_IF_FAILED(m_container->Inspect(&output));

    return wsl::shared::FromJson<docker_schema::InspectContainer>(output.get());
}
