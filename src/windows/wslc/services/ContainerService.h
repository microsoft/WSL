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
struct ContainerService
{
    static std::wstring ContainerStateToString(WSLA_CONTAINER_STATE state);
    static int Run(models::Session& session, const std::string& image, models::ContainerRunOptions options, IProgressCallback* callback);
    static models::CreateContainerResult Create(
        models::Session& session, const std::string& image, models::ContainerCreateOptions options, IProgressCallback* callback);
    static void Start(models::Session& session, const std::string& id);
    static void Stop(models::Session& session, const std::string& id, models::StopContainerOptions options);
    static void Kill(models::Session& session, const std::string& id, int signal = WSLASignalSIGKILL);
    static void Delete(models::Session& session, const std::string& id, bool force);
    static std::vector<models::ContainerInformation> List(models::Session& session);
    static int Exec(models::Session& session, const std::string& id, models::ExecContainerOptions options);
    static wsl::windows::common::docker_schema::InspectContainer Inspect(models::Session& session, const std::string& id);
};
} // namespace wsl::windows::wslc::services
