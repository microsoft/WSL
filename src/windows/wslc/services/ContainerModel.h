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
#include <string>

namespace wsl::windows::wslc::models {

// Valid formats for container list output.
enum class FormatType
{
    Table,
    Json,
};

struct ContainerOptions
{
    std::vector<std::string> Arguments;
    bool Detach = false;
    bool Interactive = false;
    std::string Name;
    bool TTY = false;
    std::vector<std::string> Volumes;
};

struct CreateContainerResult
{
    std::string Id;
};

struct StopContainerOptions
{
    static constexpr LONGLONG DefaultTimeout = -1;

    WSLASignal Signal = WSLASignalSIGTERM;
    LONGLONG Timeout = DefaultTimeout;
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

struct VolumeMount
{
    std::string HostPath() const
    {
        return m_hostPath;
    }
    std::string ContainerPath() const
    {
        return m_containerPath;
    }
    std::string Mode() const
    {
        return m_mode;
    }
    constexpr bool IsReadOnly() const
    {
        return m_mode == "ro";
    }
    static VolumeMount Parse(const std::string& value);

private:
    std::string m_hostPath;
    std::string m_containerPath;
    std::string m_mode;
};
} // namespace wsl::windows::wslc::models
