/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkService.cpp

Abstract:

    This file contains the NetworkService implementation

--*/
#include "NetworkService.h"
#include <wslutil.h>
#include <wslc.h>

using namespace wsl::shared;
using namespace wsl::shared::string;
using namespace wsl::windows::common::wslutil;

namespace wsl::windows::wslc::services {

void NetworkService::Create(models::Session& session, const models::CreateNetworkOptions& createOptions)
{
    WSLCNetworkOptions options{};
    options.Name = createOptions.Name.c_str();
    if (createOptions.Driver.has_value())
    {
        options.Driver = createOptions.Driver->c_str();
    }

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

    THROW_IF_FAILED(session.Get()->CreateNetwork(&options));
}

void NetworkService::Delete(models::Session& session, const std::string& name)
{
    THROW_IF_FAILED(session.Get()->DeleteNetwork(name.c_str()));
}

std::vector<WSLCNetworkInformation> NetworkService::List(models::Session& session)
{
    wil::unique_cotaskmem_array_ptr<WSLCNetworkInformation> rawNetworks;
    ULONG count = 0;
    THROW_IF_FAILED(session.Get()->ListNetworks(&rawNetworks, &count));

    std::vector<WSLCNetworkInformation> networks;
    networks.reserve(count);
    for (auto ptr = rawNetworks.get(), end = rawNetworks.get() + count; ptr != end; ++ptr)
    {
        networks.push_back(*ptr);
    }

    return networks;
}

wsl::windows::common::wslc_schema::InspectNetwork NetworkService::Inspect(models::Session& session, const std::string& name)
{
    wil::unique_cotaskmem_ansistring output;
    THROW_IF_FAILED(session.Get()->InspectNetwork(name.c_str(), &output));
    return FromJson<wsl::windows::common::wslc_schema::InspectNetwork>(output.get());
}
} // namespace wsl::windows::wslc::services
