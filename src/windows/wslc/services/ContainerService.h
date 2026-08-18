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
#include "Terminal.h"
#include <docker_schema.h>
#include <wslc.h>
#include <wslc_schema.h>

namespace wsl::windows::wslc::services {
struct ContainerService
{
    static std::wstring ContainerStateToString(WSLCContainerState state, ULONGLONG stateChangedAt = 0);

    // The bare state name, e.g. "running", without the relative time ContainerStateToString appends.
    // Invariant on purpose: this is what "container list --format json" reports, and it has to match
    // docker's machine readable output rather than the user's display language.
    static std::wstring ContainerStateName(WSLCContainerState state);

    // The display form of ContainerStateName, used for the table output.
    static std::wstring LocalizedContainerStateName(WSLCContainerState state);
    static std::wstring FormatRelativeTime(ULONGLONG timestamp);
    static std::wstring FormatElapsedSeconds(LONGLONG elapsedSeconds);
    static std::wstring FormatPorts(WSLCContainerState state, const std::vector<models::PortInformation>& ports);

    // Renders a container command the way docker does: optionally shortened to 20 characters with a
    // trailing ellipsis, then wrapped in double quotes.
    static std::wstring FormatCommand(const std::string& command, bool truncate);

    // Renders a container status, preferring the description supplied by the runtime and falling back
    // to a locally built one when it is unavailable.
    static std::wstring FormatStatus(const std::string& status, WSLCContainerState state, ULONGLONG stateChangedAt);
    static int Attach(Terminal& terminal, models::Session& session, const std::string& id);
    static int Run(Terminal& terminal, models::Session& session, const std::string& image, models::ContainerOptions options);
    static models::CreateContainerResult Create(Terminal& terminal, models::Session& session, const std::string& image, models::ContainerOptions options);
    static int Start(Terminal& terminal, models::Session& session, const std::string& id, bool attach = false);
    static void Stop(models::Session& session, const std::string& id, models::StopContainerOptions options);
    static void Kill(models::Session& session, const std::string& id, WSLCSignal signal = WSLCSignalSIGKILL);
    static void Delete(models::Session& session, const std::string& id, bool force, bool deleteVolumes = false);
    static std::vector<models::ContainerInformation> List(
        models::Session& session, bool all = false, int limit = -1, const std::vector<std::pair<std::string, std::string>>& filters = {});

    static int Exec(Terminal& terminal, models::Session& session, const std::string& id, models::ContainerOptions options);
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
