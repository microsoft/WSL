/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeService.h

Abstract:

    This file contains the VolumeService definition

--*/
#pragma once

#include "SessionModel.h"
#include "VolumeModel.h"
#include <wslc_schema.h>

namespace wsl::windows::wslc::services {
struct VolumeService
{
    static WSLCVolumeInformation Create(models::Session& session, const models::CreateVolumeOptions& createOptions);
    static void Delete(models::Session& session, const std::string& name);
    static std::vector<models::VolumeInformation> List(models::Session& session);
    static wsl::windows::common::wslc_schema::InspectVolume Inspect(models::Session& session, const std::string& name);
};
} // namespace wsl::windows::wslc::services
