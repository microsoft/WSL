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
#include <wslc_schema.h>

namespace wsl::windows::wslc::services {
struct ContainerService
{
    static std::wstring ContainerStateToString(WSLCContainerState state, ULONGLONG stateChangedAt = 0);
    static std::wstring FormatRelativeTime(ULONGLONG timestamp);
    static int Attach(models::Session& session, const std::string& id);
    static int Run(models::Session& session, const std::string& image, models::ContainerOptions options);
    static models::CreateContainerResult Create(models::Session& session, const std::string& image, models::ContainerOptions options);
    static int Start(models::Session& session, const std::string& id, bool attach = false);
    static void Stop(models::Session& session, const std::string& id, models::StopContainerOptions options);
    static void Kill(models::Session& session, const std::string& id, WSLCSignal signal = WSLCSignalSIGKILL);
    static void Delete(models::Session& session, const std::string& id, bool force);
    static std::vector<models::ContainerInformation> List(models::Session& session);
    static int Exec(models::Session& session, const std::string& id, models::ContainerOptions options);
    static wsl::windows::common::wslc_schema::InspectContainer Inspect(models::Session& session, const std::string& id);
    static void Logs(models::Session& session, const std::string& id, bool follow);
};
} // namespace wsl::windows::wslc::services
