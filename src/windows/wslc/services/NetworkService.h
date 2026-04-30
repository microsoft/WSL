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
#include <wslc_schema.h>

namespace wsl::windows::wslc::services {
struct NetworkService
{
    static void Create(models::Session& session, const models::CreateNetworkOptions& createOptions);
    static void Delete(models::Session& session, const std::string& name);
    static std::vector<WSLCNetworkInformation> List(models::Session& session);
    static wsl::windows::common::wslc_schema::InspectNetwork Inspect(models::Session& session, const std::string& name);
};
} // namespace wsl::windows::wslc::services
