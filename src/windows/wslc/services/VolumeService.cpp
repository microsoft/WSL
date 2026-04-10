/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeService.cpp

Abstract:

    This file contains the VolumeService implementation

--*/
#include "VolumeService.h"
#include <wslutil.h>

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;

namespace wsl::windows::wslc::services {

void VolumeService::Create(models::Session& session, const std::string& name, const std::string& type, const std::string& opt)
{
    WSLCVolumeOptions options{};
    options.Name = name.c_str();
    options.Type = type.c_str();
    options.Options = opt.c_str();
    THROW_IF_FAILED(session.Get()->CreateVolume(&options));
}

void VolumeService::Delete(models::Session& session, const std::string& name)
{
    THROW_IF_FAILED(session.Get()->DeleteVolume(name.c_str()));
}

std::vector<models::VolumeInformation> VolumeService::List(models::Session& session)
{
    wil::unique_cotaskmem_array_ptr<WSLCVolumeInformation> rawVolumes;
    ULONG count = 0;
    THROW_IF_FAILED(session.Get()->ListVolumes(&rawVolumes, &count));

    std::vector<models::VolumeInformation> volumes;
    volumes.reserve(count);
    for (auto ptr = rawVolumes.get(), end = rawVolumes.get() + count; ptr != end; ++ptr)
    {
        models::VolumeInformation info;
        info.Name = ptr->Name;
        info.Type = ptr->Type;
        volumes.push_back(std::move(info));
    }

    return volumes;
}

wsl::windows::common::wslc_schema::InspectVolume VolumeService::Inspect(models::Session& session, const std::string& name)
{
    wil::unique_cotaskmem_ansistring output;
    THROW_IF_FAILED(session.Get()->InspectVolume(name.c_str(), &output));
    return FromJson<wsl::windows::common::wslc_schema::InspectVolume>(output.get());
}

} // namespace wsl::windows::wslc::services
