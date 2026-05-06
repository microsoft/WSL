/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerLauncher.h

Abstract:

    This file contains the definition for WSLCContainerLauncher.

--*/

#pragma once
#include "WSLCProcessLauncher.h"
#include "docker_schema.h"
#include "wslc_schema.h"

namespace wsl::windows::common {

class RunningWSLCContainer
{
public:
    NON_COPYABLE(RunningWSLCContainer);
    DEFAULT_MOVABLE(RunningWSLCContainer);
    RunningWSLCContainer(wil::com_ptr<IWSLCContainer>&& Container, WSLCProcessFlags Flags);
    ~RunningWSLCContainer();
    IWSLCContainer& Get();

    WSLCContainerState State();
    ClientRunningWSLCProcess GetInitProcess();
    void SetDeleteOnClose(bool deleteOnClose);
    void Reset();
    wslc_schema::InspectContainer Inspect();
    std::string Id();
    std::string Name();
    std::map<std::string, std::string> Labels();

private:
    wil::com_ptr<IWSLCContainer> m_container;
    WSLCProcessFlags m_flags;
    bool m_deleteOnClose = true;
};

class WSLCContainerLauncher : private WSLCProcessLauncher
{
public:
    NON_COPYABLE(WSLCContainerLauncher);
    NON_MOVABLE(WSLCContainerLauncher);

    WSLCContainerLauncher(
        const std::string& Image,
        const std::string& Name = "",
        const std::vector<std::string>& Arguments = {},
        const std::vector<std::string>& Environment = {},
        WSLCContainerNetworkType containerNetworkType = WSLCContainerNetworkTypeHost,
        WSLCProcessFlags Flags = WSLCProcessFlagsNone);

    void AddVolume(const std::wstring& HostPath, const std::string& ContainerPath, bool ReadOnly);
    void AddNamedVolume(const std::string& Name, const std::string& ContainerPath, bool ReadOnly);
    void AddPort(uint16_t WindowsPort, uint16_t ContainerPort, int Family, int Protocol = IPPROTO_TCP, const std::optional<std::string>& BindingAddress = {});
    void AddLabel(const std::string& Key, const std::string& Value);
    void AddTmpfs(const std::string& ContainerPath, const std::string& Options);

    std::pair<HRESULT, std::optional<RunningWSLCContainer>> CreateNoThrow(IWSLCSession& Session);
    RunningWSLCContainer Create(IWSLCSession& Session);

    RunningWSLCContainer Launch(IWSLCSession& Session, WSLCContainerStartFlags Flags = WSLCContainerStartFlagsAttach);
    std::pair<HRESULT, std::optional<RunningWSLCContainer>> LaunchNoThrow(IWSLCSession& Session, WSLCContainerStartFlags Flags = WSLCContainerStartFlagsAttach);

    void SetName(std::string&& Name);
    void SetEntrypoint(std::vector<std::string>&& entrypoint);
    void SetDefaultStopSignal(WSLCSignal Signal);
    void SetContainerFlags(WSLCContainerFlags Flags);
    void SetContainerNetworkName(std::string&& Name);
    void SetHostname(std::string&& Hostname);
    void SetDomainname(std::string&& Domainame);
    void SetDnsServers(std::vector<std::string>&& DnsServers);
    void SetDnsSearchDomains(std::vector<std::string>&& DnsSearchDomains);
    void SetDnsOptions(std::vector<std::string>&& DnsOptions);

    using WSLCProcessLauncher::FormatResult;
    using WSLCProcessLauncher::SetUser;
    using WSLCProcessLauncher::SetWorkingDirectory;

private:
    std::string m_image;
    std::string m_name;
    std::vector<WSLCPortMapping> m_ports;
    std::vector<WSLCVolume> m_volumes;
    std::vector<WSLCNamedVolume> m_namedVolumes;
    std::deque<std::wstring> m_hostPaths;
    std::deque<std::string> m_volumeNames;
    std::deque<std::string> m_containerPaths;
    WSLCContainerNetworkType m_containerNetworkType;
    std::string m_containerNetworkName;
    std::vector<std::string> m_entrypoint;
    WSLCSignal m_stopSignal = WSLCSignalNone;
    WSLCContainerFlags m_containerFlags = WSLCContainerFlagsNone;
    std::string m_hostname;
    std::string m_domainname;
    std::vector<std::string> m_dnsServers;
    std::vector<std::string> m_dnsSearchDomains;
    std::vector<std::string> m_dnsOptions;
    std::vector<WSLCLabel> m_labels;
    std::deque<std::string> m_labelKeys;
    std::deque<std::string> m_labelValues;
    std::vector<WSLCTmpfsMount> m_tmpfsMounts;
    std::deque<std::string> m_tmpfsContainerPaths;
    std::deque<std::string> m_tmpfsOptions;
};
} // namespace wsl::windows::common