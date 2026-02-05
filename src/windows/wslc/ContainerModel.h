#pragma once

#include <wslservice.h>
#include <docker_schema.h>

namespace wslc::models
{
struct ContainerCreateOptions
{
    bool TTY = false;
    bool Interactive = false;
    std::vector<std::string> Arguments;
    std::string Name;
};

struct CreateContainerResult
{
    std::string Id;
};

struct StopContainerOptions
{
    int Signal = WSLASignalSIGTERM;
    ULONG Timeout = 5;
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
}
