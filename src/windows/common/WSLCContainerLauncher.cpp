/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerLauncher.cpp

Abstract:

    This file contains the implementation for WSLCContainerLauncher.

--*/
#include "precomp.h"
#include "WSLCContainerLauncher.h"

using wsl::windows::common::ClientRunningWSLCProcess;
using wsl::windows::common::RunningWSLCContainer;
using wsl::windows::common::WSLCContainerLauncher;

RunningWSLCContainer::RunningWSLCContainer(wil::com_ptr<IWSLCContainer>&& Container, WSLCProcessFlags Flags) :
    m_container(std::move(Container)), m_flags(Flags)
{
}

RunningWSLCContainer::~RunningWSLCContainer()
{
    Reset();
}

IWSLCContainer& RunningWSLCContainer::Get()
{
    return *m_container;
}

void RunningWSLCContainer::Reset()
{
    if (m_container && m_deleteOnClose)
    {
        // Attempt to stop and delete the container.
        LOG_IF_FAILED(m_container->Delete(WSLCDeleteFlagsForce));
    }

    m_container.reset();
}

WSLCContainerState RunningWSLCContainer::State()
{
    WSLCContainerState state{};
    THROW_IF_FAILED(m_container->GetState(&state));
    return state;
}

ClientRunningWSLCProcess RunningWSLCContainer::GetInitProcess()
{
    wil::com_ptr<IWSLCProcess> process;
    THROW_IF_FAILED(m_container->GetInitProcess(&process));

    return ClientRunningWSLCProcess{std::move(process), m_flags};
}

void RunningWSLCContainer::SetDeleteOnClose(bool deleteOnClose)
{
    m_deleteOnClose = deleteOnClose;
}

std::string RunningWSLCContainer::Id()
{
    WSLCContainerId id{};
    THROW_IF_FAILED(m_container->GetId(id));

    return id;
}

std::string RunningWSLCContainer::Name()
{
    wil::unique_cotaskmem_ansistring name;
    THROW_IF_FAILED(m_container->GetName(&name));

    return name.get();
}

WSLCContainerLauncher::WSLCContainerLauncher(
    const std::string& Image,
    const std::string& Name,
    const std::vector<std::string>& Arguments,
    const std::vector<std::string>& Environment,
    WSLCContainerNetworkType containerNetworkType,
    WSLCProcessFlags Flags) :
    WSLCProcessLauncher({}, Arguments, Environment, Flags), m_image(Image), m_name(Name), m_containerNetworkType(containerNetworkType)
{
}

void WSLCContainerLauncher::AddPort(uint16_t WindowsPort, uint16_t ContainerPort, int Family, int Protocol, const std::optional<std::string>& BindingAddress)
{
    THROW_HR_IF(E_INVALIDARG, Family != AF_INET && Family != AF_INET6);

    WSLCPortMapping port{
        .HostPort = WindowsPort,
        .ContainerPort = ContainerPort,
        .Family = Family,
        .Protocol = Protocol,
    };

    if (BindingAddress.has_value())
    {
        THROW_HR_IF(E_INVALIDARG, BindingAddress->size() > WSLC_MAX_BINDING_ADDRESS_LENGTH);
        THROW_HR_IF_MSG(
            E_INVALIDARG, strcpy_s(port.BindingAddress, BindingAddress->c_str()) != 0, "Invalid address: %hs", BindingAddress->c_str());
    }
    else
    {
        static_assert(sizeof("127.0.0.1") <= WSLC_MAX_BINDING_ADDRESS_LENGTH + 1, "Default IPv4 binding address too long");
        static_assert(sizeof("::1") <= WSLC_MAX_BINDING_ADDRESS_LENGTH + 1, "Default IPv6 binding address too long");
        THROW_HR_IF(E_INVALIDARG, strcpy_s(port.BindingAddress, Family == AF_INET ? "127.0.0.1" : "::1") != 0);
    }

    m_ports.push_back(port);
}

void WSLCContainerLauncher::SetName(std::string&& Name)
{
    m_name = std::move(Name);
}

void WSLCContainerLauncher::SetDefaultStopSignal(WSLCSignal Signal)
{
    m_stopSignal = Signal;
}

void WSLCContainerLauncher::SetEntrypoint(std::vector<std::string>&& entrypoint)
{
    m_entrypoint = std::move(entrypoint);
}

void WSLCContainerLauncher::SetContainerFlags(WSLCContainerFlags Flags)
{
    m_containerFlags = Flags;
}

void WSLCContainerLauncher::SetHostname(std::string&& Hostname)
{
    m_hostname = std::move(Hostname);
}

void WSLCContainerLauncher::SetDomainname(std::string&& Domainame)
{
    m_domainname = std::move(Domainame);
}

void WSLCContainerLauncher::SetDnsServers(std::vector<std::string>&& DnsServers)
{
    m_dnsServers = std::move(DnsServers);
}

void WSLCContainerLauncher::SetDnsSearchDomains(std::vector<std::string>&& DnsSearchDomains)
{
    m_dnsSearchDomains = std::move(DnsSearchDomains);
}

void WSLCContainerLauncher::SetDnsOptions(std::vector<std::string>&& DnsOptions)
{
    m_dnsOptions = std::move(DnsOptions);
}

void wsl::windows::common::WSLCContainerLauncher::AddVolume(const std::wstring& HostPath, const std::string& ContainerPath, bool ReadOnly)
{
    // Store a copy of the path strings to the launcher to ensure the pointers in WSLCVolume remain valid.
    const auto& hostPath = m_hostPaths.emplace_back(HostPath);
    const auto& containerPath = m_containerPaths.emplace_back(ContainerPath);

    WSLCVolume vol{};
    vol.HostPath = hostPath.c_str();
    vol.ContainerPath = containerPath.c_str();
    vol.ReadOnly = ReadOnly ? TRUE : FALSE;

    m_volumes.push_back(vol);
}

void wsl::windows::common::WSLCContainerLauncher::AddNamedVolume(const std::string& Name, const std::string& ContainerPath, bool ReadOnly)
{
    const auto& name = m_volumeNames.emplace_back(Name);
    const auto& containerPath = m_containerPaths.emplace_back(ContainerPath);

    WSLCNamedVolume volume{};
    volume.Name = name.c_str();
    volume.ContainerPath = containerPath.c_str();
    volume.ReadOnly = ReadOnly ? TRUE : FALSE;

    m_namedVolumes.push_back(volume);
}

void wsl::windows::common::WSLCContainerLauncher::AddLabel(const std::string& Key, const std::string& Value)
{
    // Store a copy of the key/value strings to the launcher to ensure the pointers in WSLCLabel remain valid.
    const auto& key = m_labelKeys.emplace_back(Key);
    const auto& value = m_labelValues.emplace_back(Value);

    WSLCLabel label{};
    label.Key = key.c_str();
    label.Value = value.c_str();

    m_labels.push_back(label);
}

void wsl::windows::common::WSLCContainerLauncher::AddTmpfs(const std::string& ContainerPath, const std::string& Options)
{
    // Store a copy of the path/options strings to the launcher to ensure the pointers in WSLCTmpfsMount remain valid.
    const auto& containerPath = m_tmpfsContainerPaths.emplace_back(ContainerPath);
    const auto& options = m_tmpfsOptions.emplace_back(Options);

    WSLCTmpfsMount tmpfs{};
    tmpfs.Destination = containerPath.c_str();
    tmpfs.Options = options.c_str();

    m_tmpfsMounts.push_back(tmpfs);
}

std::pair<HRESULT, std::optional<RunningWSLCContainer>> WSLCContainerLauncher::LaunchNoThrow(IWSLCSession& Session, WSLCContainerStartFlags Flags)
{
    auto [result, container] = CreateNoThrow(Session);
    if (FAILED(result))
    {
        return std::make_pair(result, std::optional<RunningWSLCContainer>{});
    }

    result = container.value().Get().Start(Flags, nullptr);

    return std::make_pair(result, std::move(container));
}

std::pair<HRESULT, std::optional<RunningWSLCContainer>> WSLCContainerLauncher::CreateNoThrow(IWSLCSession& Session)
{
    WSLCContainerOptions options{};
    options.Image = m_image.c_str();

    if (!m_name.empty())
    {
        options.Name = m_name.c_str();
    }

    std::vector<const char*> entrypointStorage;

    for (const auto& e : m_entrypoint)
    {
        entrypointStorage.push_back(e.c_str());
    }

    auto [processOptions, commandLinePtrs, environmentPtrs] = CreateProcessOptions();
    options.InitProcessOptions = processOptions;
    options.ContainerNetwork.ContainerNetworkType = m_containerNetworkType;
    options.Ports = m_ports.data();
    options.PortsCount = static_cast<ULONG>(m_ports.size());
    options.StopSignal = m_stopSignal;
    options.Flags = m_containerFlags;

    if (!entrypointStorage.empty())
    {
        options.Entrypoint = {entrypointStorage.data(), static_cast<ULONG>(entrypointStorage.size())};
    }

    if (!m_hostname.empty())
    {
        options.HostName = m_hostname.c_str();
    }

    if (!m_domainname.empty())
    {
        options.DomainName = m_domainname.c_str();
    }

    std::vector<const char*> dnsServersStorage;
    for (const auto& e : m_dnsServers)
    {
        dnsServersStorage.push_back(e.c_str());
    }

    if (!dnsServersStorage.empty())
    {
        options.DnsServers = {dnsServersStorage.data(), static_cast<ULONG>(dnsServersStorage.size())};
    }

    std::vector<const char*> dnsSearchDomainsStorage;
    for (const auto& e : m_dnsSearchDomains)
    {
        dnsSearchDomainsStorage.push_back(e.c_str());
    }

    if (!dnsSearchDomainsStorage.empty())
    {
        options.DnsSearchDomains = {dnsSearchDomainsStorage.data(), static_cast<ULONG>(dnsSearchDomainsStorage.size())};
    }

    std::vector<const char*> dnsOptionsStorage;
    for (const auto& e : m_dnsOptions)
    {
        dnsOptionsStorage.push_back(e.c_str());
    }

    if (!dnsOptionsStorage.empty())
    {
        options.DnsOptions = {dnsOptionsStorage.data(), static_cast<ULONG>(dnsOptionsStorage.size())};
    }

    if (!m_workingDirectory.empty())
    {
        options.InitProcessOptions.CurrentDirectory = m_workingDirectory.c_str();
    }

    options.VolumesCount = static_cast<ULONG>(m_volumes.size());
    options.Volumes = m_volumes.size() > 0 ? m_volumes.data() : nullptr;

    options.NamedVolumesCount = static_cast<ULONG>(m_namedVolumes.size());
    options.NamedVolumes = m_namedVolumes.size() > 0 ? m_namedVolumes.data() : nullptr;

    options.LabelsCount = static_cast<ULONG>(m_labels.size());
    options.Labels = m_labels.size() > 0 ? m_labels.data() : nullptr;

    options.TmpfsCount = static_cast<ULONG>(m_tmpfsMounts.size());
    options.Tmpfs = m_tmpfsMounts.size() > 0 ? m_tmpfsMounts.data() : nullptr;

    // TODO: Support volumes, ports, flags, shm size, container networking mode, etc.
    wil::com_ptr<IWSLCContainer> container;
    auto result = Session.CreateContainer(&options, &container);
    if (FAILED(result))
    {
        return std::pair<HRESULT, std::optional<RunningWSLCContainer>>(result, std::optional<RunningWSLCContainer>{});
    }

    return std::make_pair(S_OK, std::move(RunningWSLCContainer{std::move(container), m_flags}));
}

RunningWSLCContainer WSLCContainerLauncher::Create(IWSLCSession& Session)
{
    auto [result, container] = CreateNoThrow(Session);
    THROW_IF_FAILED(result);

    return std::move(container.value());
}

RunningWSLCContainer WSLCContainerLauncher::Launch(IWSLCSession& Session, WSLCContainerStartFlags Flags)
{
    auto [result, container] = LaunchNoThrow(Session, Flags);
    THROW_IF_FAILED(result);

    return std::move(container.value());
}

wsl::windows::common::wslc_schema::InspectContainer RunningWSLCContainer::Inspect()
{
    wil::unique_cotaskmem_ansistring output;
    THROW_IF_FAILED(m_container->Inspect(&output));

    return wsl::shared::FromJson<wslc_schema::InspectContainer>(output.get());
}

std::map<std::string, std::string> RunningWSLCContainer::Labels()
{
    wil::unique_cotaskmem_array_ptr<WSLCLabelInformation> labels;
    THROW_IF_FAILED(m_container->GetLabels(&labels, labels.size_address<ULONG>()));

    std::map<std::string, std::string> result;
    for (size_t i = 0; i < labels.size(); i++)
    {
        result[labels[i].Key] = labels[i].Value;
        CoTaskMemFree(labels[i].Key);
        CoTaskMemFree(labels[i].Value);
    }

    return result;
}
