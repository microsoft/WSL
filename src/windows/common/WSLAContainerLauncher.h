/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAContainerLauncher.h

Abstract:

    This file contains the definition for WSLAContainerLauncher.

--*/

#pragma once
#include "WSLAProcessLauncher.h"
#include "docker_schema.h"
#include "wsla_schema.h"

namespace wsl::windows::common {

class RunningWSLAContainer
{
public:
    NON_COPYABLE(RunningWSLAContainer);
    DEFAULT_MOVABLE(RunningWSLAContainer);
    RunningWSLAContainer(wil::com_ptr<IWSLAContainer>&& Container, WSLAProcessFlags Flags);
    ~RunningWSLAContainer();
    IWSLAContainer& Get();

    WSLA_CONTAINER_STATE State();
    ClientRunningWSLAProcess GetInitProcess();
    void SetDeleteOnClose(bool deleteOnClose);
    void Reset();
    wsla_schema::InspectContainer Inspect();
    std::string Id();
    std::string Name();
    std::map<std::string, std::string> Labels();

private:
    wil::com_ptr<IWSLAContainer> m_container;
    WSLAProcessFlags m_flags;
    bool m_deleteOnClose = true;
};

class WSLAContainerLauncher : private WSLAProcessLauncher
{
public:
    NON_COPYABLE(WSLAContainerLauncher);
    NON_MOVABLE(WSLAContainerLauncher);

    WSLAContainerLauncher(
        const std::string& Image,
        const std::string& Name = "",
        const std::vector<std::string>& Arguments = {},
        const std::vector<std::string>& Environment = {},
        WSLA_CONTAINER_NETWORK_TYPE containerNetworkType = WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_HOST,
        WSLAProcessFlags Flags = WSLAProcessFlagsNone);

    void AddVolume(const std::wstring& HostPath, const std::string& ContainerPath, bool ReadOnly);
    void AddPort(uint16_t WindowsPort, uint16_t ContainerPort, int Family);
    void AddLabel(const std::string& Key, const std::string& Value);

    std::pair<HRESULT, std::optional<RunningWSLAContainer>> CreateNoThrow(IWSLASession& Session);
    RunningWSLAContainer Create(IWSLASession& Session);

    RunningWSLAContainer Launch(IWSLASession& Session, WSLAContainerStartFlags Flags = WSLAContainerStartFlagsAttach);
    std::pair<HRESULT, std::optional<RunningWSLAContainer>> LaunchNoThrow(IWSLASession& Session, WSLAContainerStartFlags Flags = WSLAContainerStartFlagsAttach);

    void SetEntrypoint(std::vector<std::string>&& entrypoint);
    void SetDefaultStopSignal(WSLASignal Signal);
    void SetContainerFlags(WSLAContainerFlags Flags);
    void SetHostname(std::string&& Hostname);
    void SetDomainname(std::string&& Domainame);
    void SetDnsServers(std::vector<std::string>&& DnsServers);
    void SetDnsSearchDomains(std::vector<std::string>&& DnsSearchDomains);

    using WSLAProcessLauncher::SetUser;
    using WSLAProcessLauncher::SetWorkingDirectory;

private:
    std::string m_image;
    std::string m_name;
    std::vector<WSLA_PORT_MAPPING> m_ports;
    std::vector<WSLA_VOLUME> m_volumes;
    std::deque<std::wstring> m_hostPaths;
    std::deque<std::string> m_containerPaths;
    WSLA_CONTAINER_NETWORK_TYPE m_containerNetworkType;
    std::vector<std::string> m_entrypoint;
    WSLASignal m_stopSignal = WSLASignalNone;
    WSLAContainerFlags m_containerFlags = WSLAContainerFlagsNone;
    std::string m_hostname;
    std::string m_domainname;
    std::vector<std::string> m_dnsServers;
    std::vector<std::string> m_dnsSearchDomains;
    std::vector<WSLA_LABEL> m_labels;
    std::deque<std::string> m_labelKeys;
    std::deque<std::string> m_labelValues;
};
} // namespace wsl::windows::common