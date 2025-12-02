/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAContainerLauncher.h

Abstract:

    This file contains the definition for WSLAContainerLauncher.

--*/

#pragma once
#include "WSLAProcessLauncher.h"

namespace wsl::windows::common {

class RunningWSLAContainer
{
public:
    NON_COPYABLE(RunningWSLAContainer);
    DEFAULT_MOVABLE(RunningWSLAContainer);
    RunningWSLAContainer(wil::com_ptr<IWSLAContainer>&& Container, std::vector<WSLA_PROCESS_FD>&& fds);
    IWSLAContainer& Get();

    WSLA_CONTAINER_STATE State();
    ClientRunningWSLAProcess GetInitProcess();

private:
    wil::com_ptr<IWSLAContainer> m_container;
    std::vector<WSLA_PROCESS_FD> m_fds;
};

class WSLAContainerLauncher : public WSLAProcessLauncher
{
public:
    NON_COPYABLE(WSLAContainerLauncher);
    NON_MOVABLE(WSLAContainerLauncher);

    WSLAContainerLauncher(
        const std::string& Image,
        const std::string& Name,
        const std::string& EntryPoint = "",
        const std::vector<std::string>& Arguments = {},
        const std::vector<std::string>& Environment = {},
        ProcessFlags Flags = ProcessFlags::Stdout | ProcessFlags::Stderr);

    void AddVolume(const std::string& HostPath, const std::string& ContainerPath, bool ReadOnly);
    void AddPort(uint16_t WindowsPort, uint16_t ContainerPort, int Family);

    RunningWSLAContainer Launch(IWSLASession& Session);

private:
    std::string m_image;
    std::string m_name;
};
} // namespace wsl::windows::common