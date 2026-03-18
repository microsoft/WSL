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
    bool Remove = false;
    bool TTY = false;
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

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ContainerInformation, Id, Name, Image, State, StateChangedAt, CreatedAt);
};
} // namespace wsl::windows::wslc::models
