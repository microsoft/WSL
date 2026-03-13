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

struct ContainerOptions
{
    std::vector<std::string> Arguments;
    bool Detach = false;
    bool Interactive = false;
    std::string Name;
    bool TTY = false;
    std::vector<std::wstring> Volumes;
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

struct ContainerInformation
{
    std::string Id;
    std::string Name;
    std::string Image;
    WSLAContainerState State;
    ULONGLONG StateChangedAt{};
    ULONGLONG CreatedAt{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(ContainerInformation, Id, Name, Image, State, StateChangedAt, CreatedAt);
};

struct VolumeMount
{
    std::wstring HostPath() const
    {
        return m_hostPath;
    }

    std::string ContainerPath() const
    {
        return m_containerPath;
    }

    bool IsReadOnly() const
    {
        return m_isReadOnlyMode;
    }
    static VolumeMount Parse(const std::wstring& value);

private:
    std::wstring m_hostPath;
    std::string m_containerPath;
    bool m_isReadOnlyMode = false;

    static bool IsReadOnlyMode(const std::wstring& mode)
    {
        return mode == L"ro" || mode == L"readonly";
    }

    static bool IsValidMode(const std::wstring& mode)
    {
        return IsReadOnlyMode(mode) || mode == L"rw";
    }
};
} // namespace wsl::windows::wslc::models
