/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerModel.h

Abstract:

    This file contains the ContainerModel definitions

--*/

#pragma once

#include <wslservice.h>
#include <wslaservice.h>
#include <string>

namespace wsl::windows::wslc::models {

// Valid formats for container list output.
enum class FormatType
{
    Table,
    Json,
};

struct PortOption
{
    uint16_t HostPort{};
    uint16_t ContainerPort{};
    int Family = AF_INET;
};

struct ContainerOptions
{
    std::vector<std::string> Arguments;
    bool Detach = false;
    bool Interactive = false;
    std::string Name;
    bool Remove = false;
    bool TTY = false;
    std::vector<PortOption> Ports;
};

struct CreateContainerResult
{
    std::string Id;
};

struct StopContainerOptions
{
    static constexpr LONG DefaultTimeout = -1;

    WSLASignal Signal = WSLASignalSIGTERM;
    LONG Timeout = DefaultTimeout;
};

struct KillContainerOptions
{
    int Signal = WSLASignalSIGKILL;
};

struct PortInformation
{
    uint16_t HostPort{};
    uint16_t ContainerPort{};
    int Family{};   // AF_INET or AF_INET6
    int Protocol{}; // 0 = TCP, 1 = UDP

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(PortInformation, HostPort, ContainerPort, Family, Protocol);
};

struct ContainerInformation
{
    std::string Id;
    std::string Name;
    std::string Image;
    WSLAContainerState State;
    ULONGLONG StateChangedAt{};
    ULONGLONG CreatedAt{};
    std::vector<PortInformation> Ports;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(ContainerInformation, Id, Name, Image, State, StateChangedAt, CreatedAt, Ports);
};
} // namespace wsl::windows::wslc::models
