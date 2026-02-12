/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerService.h

Abstract:

    This file contains the ContainerService definition

--*/
#pragma once
#include "SessionModel.h"
#include "ContainerModel.h"

namespace wslc::services {
class ContainerService
{
public:
    int Run(wslc::models::Session& session, std::string image, wslc::models::ContainerRunOptions options);
    wslc::models::CreateContainerResult Create(wslc::models::Session& session, std::string image, wslc::models::ContainerCreateOptions options);
    void Start(wslc::models::Session& session, std::string id);
    void Stop(wslc::models::Session& session, std::string id, wslc::models::StopContainerOptions options);
    void Kill(wslc::models::Session& session, std::string id, int signal = WSLASignalSIGKILL);
    void Delete(wslc::models::Session& session, std::string id, bool force);
    std::vector<wslc::models::ContainerInformation> List(wslc::models::Session& session);
    int Exec(wslc::models::Session& session, std::string id, wslc::models::ExecContainerOptions options);
    wsl::windows::common::docker_schema::InspectContainer Inspect(wslc::models::Session& session, std::string id);
};
} // namespace wslc::services