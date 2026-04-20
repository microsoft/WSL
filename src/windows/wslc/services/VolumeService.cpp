/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeService.cpp

Abstract:

    This file contains the VolumeService implementation

--*/
#include "VolumeService.h"
#include <wslutil.h>
#include <wslc.h>

using namespace wsl::shared;
using namespace wsl::shared::string;
using namespace wsl::windows::common::wslutil;

namespace wsl::windows::wslc::services {

WSLCVolumeInformation VolumeService::Create(models::Session& session, const models::CreateVolumeOptions& createOptions)
{
    WSLCVolumeOptions options{};
    options.Name = createOptions.Name.c_str();
    options.Driver = createOptions.Driver.c_str();

    // Set driver options
    std::vector<KeyValuePair> driverOpts;
    for (const auto& option : createOptions.DriverOpts)
    {
        driverOpts.push_back({.Key = option.first.c_str(), .Value = option.second.c_str()});
    }

    // Set labels
    std::vector<KeyValuePair> labels;
    for (const auto& label : createOptions.Labels)
    {
        labels.push_back({.Key = label.first.c_str(), .Value = label.second.c_str()});
    }

    options.DriverOpts = driverOpts.data();
    options.DriverOptsCount = static_cast<ULONG>(driverOpts.size());
    options.Labels = labels.data();
    options.LabelsCount = static_cast<ULONG>(labels.size());

    WSLCVolumeInformation info{};
    THROW_IF_FAILED(session.Get()->CreateVolume(&options, &info));
    return info;
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
        info.Driver = ptr->Driver;
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
