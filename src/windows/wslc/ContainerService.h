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

namespace wsl::windows::wslc::services {
class ContainerService
{
public:
    int Run(models::Session& session, const std::string& image, models::ContainerRunOptions options, IProgressCallback* callback);
    models::CreateContainerResult Create(models::Session& session, const std::string& image, models::ContainerCreateOptions options, IProgressCallback* callback);
    void Start(models::Session& session, const std::string& id);
    void Stop(models::Session& session, const std::string& id, models::StopContainerOptions options);
    void Kill(models::Session& session, const std::string& id, int signal = WSLASignalSIGKILL);
    void Delete(models::Session& session, const std::string& id, bool force);
    std::vector<models::ContainerInformation> List(models::Session& session);
    int Exec(models::Session& session, const std::string& id, models::ExecContainerOptions options);
    wsl::windows::common::docker_schema::InspectContainer Inspect(models::Session& session, const std::string& id);
};
} // namespace wsl::windows::wslc::services