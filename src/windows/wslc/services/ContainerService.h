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
#include "Reporter.h"
#include <docker_schema.h>
#include <wslc.h>
#include <wslc_schema.h>

namespace wsl::windows::wslc::services {
struct ContainerService
{
    static std::wstring ContainerStateToString(WSLCContainerState state, ULONGLONG stateChangedAt = 0);
    static std::wstring FormatRelativeTime(ULONGLONG timestamp);
    static std::wstring FormatPorts(WSLCContainerState state, const std::vector<models::PortInformation>& ports);
    static int Attach(Reporter& reporter, models::Session& session, const std::string& id);
    static int Run(Reporter& reporter, models::Session& session, const std::string& image, models::ContainerOptions options);
    static models::CreateContainerResult Create(Reporter& reporter, models::Session& session, const std::string& image, models::ContainerOptions options);
    static int Start(Reporter& reporter, models::Session& session, const std::string& id, bool attach = false);
    static void Stop(models::Session& session, const std::string& id, models::StopContainerOptions options);
    static void Kill(models::Session& session, const std::string& id, WSLCSignal signal = WSLCSignalSIGKILL);
    static void Delete(models::Session& session, const std::string& id, bool force);
    static std::vector<models::ContainerInformation> List(
        models::Session& session, bool all = false, int limit = -1, const std::vector<std::pair<std::string, std::string>>& filters = {});

    static int Exec(Reporter& reporter, models::Session& session, const std::string& id, models::ContainerOptions options);
    static void Export(models::Session& session, const std::string& id, const std::wstring& outputPath);
    static void Export(models::Session& session, const std::string& id, HANDLE outputHandle);
    static void CopyToContainer(models::Session& session, const std::string& id, const std::string& destPath, HANDLE inputHandle, ULONGLONG contentSize);
    static void CopyFromContainer(models::Session& session, const std::string& id, const std::string& srcPath, HANDLE outputHandle);
    static wsl::windows::common::wslc_schema::InspectContainer Inspect(models::Session& session, const std::string& id);
    static void Logs(models::Session& session, const std::string& id, bool follow, bool timestamps, ULONGLONG since, ULONGLONG until, ULONGLONG tail = 0);
    static wsl::windows::common::docker_schema::ContainerStats Stats(models::Session& session, const std::string& id);
    static models::PruneContainersResult Prune(models::Session& session);
};
} // namespace wsl::windows::wslc::services
