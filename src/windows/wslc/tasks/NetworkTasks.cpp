/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkTasks.cpp

Abstract:

    Implementation of network command related execution logic.

--*/
#include "Argument.h"
#include "ArgumentValidation.h"
#include "CLIExecutionContext.h"
#include "NetworkModel.h"
#include "NetworkService.h"
#include "NetworkTasks.h"
#include "TableOutput.h"
#include <wslc_schema.h>

using namespace wsl::shared;
using namespace wsl::windows::common;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::models;
using namespace wsl::windows::wslc::services;

namespace wsl::windows::wslc::task {

static bool TryInspectNetwork(Reporter& reporter, Session& session, const std::string& networkName, std::optional<wslc_schema::Network>& inspectData)
{
    try
    {
        inspectData = NetworkService::Inspect(session, networkName);
        return true;
    }
    catch (const wil::ResultException& ex)
    {
        if (ex.GetErrorCode() == WSLC_E_NETWORK_NOT_FOUND)
        {
            reporter.Error(L"{}\n", Localization::MessageWslcNetworkNotFound(networkName.c_str()));
            return false;
        }

        throw;
    }
}

static bool TryDeleteNetwork(Reporter& reporter, Session& session, const std::string& networkName, bool force)
{
    try
    {
        NetworkService::Delete(session, networkName);
        return true;
    }
    catch (const wil::ResultException& ex)
    {
        if (ex.GetErrorCode() == WSLC_E_NETWORK_NOT_FOUND)
        {
            if (!force)
            {
                reporter.Error(L"{}\n", Localization::MessageWslcNetworkNotFound(networkName.c_str()));
            }

            return false;
        }

        throw;
    }
}

void CreateNetwork(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::NetworkName));

    models::CreateNetworkOptions options{};
    options.Name = WideToMultiByte(context.Args.Get<ArgType::NetworkName>());

    for (const auto& option : context.Args.GetAll<ArgType::Options>())
    {
        options.DriverOpts.push_back(validation::ParseDriverOption(option));
    }

    for (const auto& label : context.Args.GetAll<ArgType::Label>())
    {
        options.Labels.push_back(validation::ParseLabel(label));
    }

    if (context.Args.Contains(ArgType::Driver))
    {
        options.Driver = WideToMultiByte(context.Args.Get<ArgType::Driver>());
    }

    options.Internal = context.Args.Contains(ArgType::Internal);

    if (context.Args.Contains(ArgType::Subnet))
    {
        options.Subnet = WideToMultiByte(context.Args.Get<ArgType::Subnet>());
    }

    if (context.Args.Contains(ArgType::Gateway))
    {
        options.Gateway = WideToMultiByte(context.Args.Get<ArgType::Gateway>());
    }

    NetworkService::Create(context.Reporter, context.Data.Get<Data::Session>(), options);
    context.Reporter.Output(L"{}\n", MultiByteToWide(options.Name));
}

void DeleteNetworks(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto networkNames = context.Args.GetAll<ArgType::NetworkName>();
    const bool force = context.Args.Contains(ArgType::Force);
    for (const auto& name : networkNames)
    {
        if (TryDeleteNetwork(context.Reporter, session, WideToMultiByte(name), force))
        {
            context.Reporter.Output(L"{}\n", name);
        }
        else if (!force)
        {
            context.ExitCode = 1;
        }
    }
}

void GetNetworks(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    context.Data.Add<Data::Networks>(NetworkService::List(session));
}

void InspectNetworks(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto networkNames = context.Args.GetAll<ArgType::NetworkName>();
    std::vector<wsl::windows::common::wslc_schema::Network> result;
    for (const auto& name : networkNames)
    {
        std::optional<wslc_schema::Network> inspectData;
        if (TryInspectNetwork(context.Reporter, session, WideToMultiByte(name), inspectData))
        {
            result.push_back(*inspectData);
        }
        else
        {
            context.ExitCode = 1;
        }
    }

    auto json = ToJson(result, c_jsonPrettyPrintIndent);
    context.Reporter.Output(L"{}\n", MultiByteToWide(json));
}

void ListNetworks(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Networks));
    auto& networks = context.Data.Get<Data::Networks>();

    if (context.Args.Contains(ArgType::Quiet))
    {
        for (const auto& network : networks)
        {
            context.Reporter.Output(L"{}\n", MultiByteToWide(network.Name));
        }

        return;
    }

    FormatType format = FormatType::Table;
    if (context.Args.Contains(ArgType::Format))
    {
        format = validation::GetFormatTypeFromString(context.Args.Get<ArgType::Format>());
    }

    switch (format)
    {
    case FormatType::Json:
    {
        auto json = ToJson(networks, c_jsonPrettyPrintIndent);
        context.Reporter.Output(L"{}\n", MultiByteToWide(json));
        break;
    }
    case FormatType::Table:
    {
        auto table = wsl::windows::wslc::TableOutput<3>(context.Reporter, {L"NETWORK ID", L"NAME", L"DRIVER"});
        for (const auto& network : networks)
        {
            table.WriteRow({
                MultiByteToWide(TruncateId(network.Id)),
                MultiByteToWide(network.Name),
                MultiByteToWide(network.Driver),
            });
        }

        table.Complete();
        break;
    }
    default:
        THROW_HR(E_UNEXPECTED);
    }
}

void PruneNetworks(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();

    std::vector<std::pair<std::string, std::string>> filters;
    for (const auto& value : context.Args.GetAll<ArgType::Filter>())
    {
        filters.push_back(validation::ParseFilter(value));
    }

    auto result = NetworkService::Prune(session, filters);

    for (const auto& networkName : result.PrunedNetworks)
    {
        context.Reporter.Output(L"{}\n", Localization::WSLCCLI_NetworkPruneDeleted(MultiByteToWide(networkName)));
    }
}

void ConnectNetwork(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::NetworkName));
    WI_ASSERT(context.Args.Contains(ArgType::ContainerId));

    const auto networkName = WideToMultiByte(context.Args.Get<ArgType::NetworkName>());
    const auto containerId = WideToMultiByte(context.Args.Get<ArgType::ContainerId>());
    NetworkService::Connect(context.Data.Get<Data::Session>(), networkName, containerId);
}

void DisconnectNetwork(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::NetworkName));
    WI_ASSERT(context.Args.Contains(ArgType::ContainerId));

    const auto networkName = WideToMultiByte(context.Args.Get<ArgType::NetworkName>());
    const auto containerId = WideToMultiByte(context.Args.Get<ArgType::ContainerId>());
    NetworkService::Disconnect(context.Data.Get<Data::Session>(), networkName, containerId);
}
} // namespace wsl::windows::wslc::task
