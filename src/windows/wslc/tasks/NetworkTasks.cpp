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

static bool TryInspectNetwork(Reporter& output, Session& session, const std::string& networkName, std::optional<wslc_schema::Network>& inspectData)
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
            output.Error() << Localization::MessageWslcNetworkNotFound(networkName.c_str()) << std::endl;
            return false;
        }

        throw;
    }
}

static bool TryDeleteNetwork(Reporter& output, Session& session, const std::string& networkName)
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
            output.Error() << Localization::MessageWslcNetworkNotFound(networkName.c_str()) << std::endl;
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

    NetworkService::Create(context.Data.Get<Data::Session>(), options);
    context.Reporter.Output() << MultiByteToWide(options.Name) << std::endl;
}

void DeleteNetworks(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto networkNames = context.Args.GetAll<ArgType::NetworkName>();
    for (const auto& name : networkNames)
    {
        if (TryDeleteNetwork(context.Reporter, session, WideToMultiByte(name)))
        {
            context.Reporter.Output() << name << std::endl;
        }
        else
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
    context.Reporter.Output() << MultiByteToWide(json) << std::endl;
}

void ListNetworks(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Networks));
    auto& networks = context.Data.Get<Data::Networks>();

    if (context.Args.Contains(ArgType::Quiet))
    {
        for (const auto& network : networks)
        {
            context.Reporter.Output() << MultiByteToWide(network.Name) << std::endl;
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
        context.Reporter.Output() << MultiByteToWide(json) << std::endl;
        break;
    }
    case FormatType::Table:
    {
        auto table = wsl::windows::wslc::TableOutput<3>({L"NETWORK ID", L"NAME", L"DRIVER"});
        for (const auto& network : networks)
        {
            table.OutputLine({
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
} // namespace wsl::windows::wslc::task
