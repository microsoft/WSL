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
#include <docker_schema.h>

namespace wsl::windows::wslc::models {
struct ContainerCreateOptions
{
    bool TTY = false;
    bool Interactive = false;
    std::vector<std::string> Arguments;
    std::string Name;
    std::string Port;
};

struct ContainerRunOptions : public ContainerCreateOptions
{
    bool Detach = false;
};

struct CreateContainerResult
{
    std::string Id;
};

struct StopContainerOptions
{
    static constexpr ULONG DefaultTimeout = -1;

    int Signal = WSLASignalSIGTERM;
    ULONG Timeout = DefaultTimeout;
};

struct KillContainerOptions
{
    int Signal = WSLASignalSIGKILL;
};

struct ContainerInformation
{
    std::string Id;
    std::string Name;
    std::string Image;
    WSLA_CONTAINER_STATE State;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(ContainerInformation, Id, Name, Image, State);
};

struct ExecContainerOptions
{
    bool TTY = false;
    bool Interactive = false;
    std::vector<std::string> Arguments;
};

struct PublishPort
{
    enum class Protocol
    {
        UDP,
        TCP,
    };

    struct PortRange
    {
        int Start;
        int End;

        constexpr unsigned int Count() const noexcept { return (End >= Start) ? (End - Start + 1) : 0; }
        constexpr bool IsSingle() const noexcept { return Count() == 1; }
        constexpr bool IsValid() const noexcept { return Count() > 0 && IsValidPort(Start) && IsValidPort(End); }
    };

    struct IPAddress
    {
        std::string Value;
        bool IsIPv6 = false;
    };

    std::optional<IPAddress> HostIP;
    std::optional<PortRange> HostPort;
    PortRange ContainerPort;
    Protocol PortProtocol = Protocol::TCP;
    std::string Original;

    static PublishPort Parse(const std::string& value);
    static constexpr bool IsValidPort(int port) noexcept { return port >= 1 && port <= 65535; }
    bool HasEphemeralHostPort() const noexcept { return !HostPort.has_value(); }
    bool IsRangeMapping() const noexcept { return !ContainerPort.IsSingle(); }
private:
    void Validate() const;
    PublishPort() = default;
};

} // namespace wsl::windows::wslc::models
