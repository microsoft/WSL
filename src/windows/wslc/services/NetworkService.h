/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkService.h

Abstract:

    This file contains the NetworkService definition

--*/
#pragma once

#include "SessionModel.h"
#include "NetworkModel.h"
#include "Reporter.h"
#include <wslc.h>
#include <wslc_schema.h>

namespace wsl::windows::wslc::services {
struct NetworkService
{
    static void Create(Reporter& reporter, models::Session& session, const models::CreateNetworkOptions& createOptions);
    static void Delete(models::Session& session, const std::string& name);
    static std::vector<WSLCNetworkInformation> List(models::Session& session);
    static wsl::windows::common::wslc_schema::Network Inspect(models::Session& session, const std::string& name);
    static models::PruneNetworksResult Prune(models::Session& session, const std::vector<std::pair<std::string, std::string>>& filters = {});
    static void Connect(models::Session& session, const std::string& networkName, const std::string& containerId);
    static void Disconnect(models::Session& session, const std::string& networkName, const std::string& containerId);
};
} // namespace wsl::windows::wslc::services
