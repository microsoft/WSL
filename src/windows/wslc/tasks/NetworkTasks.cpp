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

static std::pair<std::string, std::string> OptionsToKeyValue(const std::wstring& option)
{
    auto pos = option.find('=');
    if (pos == std::wstring::npos)
    {
        return {WideToMultiByte(option), std::string()};
    }

    return {WideToMultiByte(option.substr(0, pos)), WideToMultiByte(option.substr(pos + 1))};
}

static bool TryInspectNetwork(Session& session, const std::string& networkName, std::optional<wslc_schema::InspectNetwork>& inspectData)
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
            PrintMessage(Localization::MessageWslcNetworkNotFound(networkName.c_str()), stderr);
            return false;
        }

        throw;
    }
}

static bool TryDeleteNetwork(Session& session, const std::string& networkName)
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
            PrintMessage(Localization::MessageWslcNetworkNotFound(networkName.c_str()), stderr);
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
        options.DriverOpts.push_back(OptionsToKeyValue(option));
    }

    for (const auto& label : context.Args.GetAll<ArgType::Label>())
    {
        options.Labels.push_back(OptionsToKeyValue(label));
    }

    if (context.Args.Contains(ArgType::Driver))
    {
        options.Driver = WideToMultiByte(context.Args.Get<ArgType::Driver>());
    }

    NetworkService::Create(context.Data.Get<Data::Session>(), options);
    PrintMessage(MultiByteToWide(options.Name));
}

void DeleteNetworks(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto networkNames = context.Args.GetAll<ArgType::NetworkName>();
    for (const auto& name : networkNames)
    {
        if (TryDeleteNetwork(session, WideToMultiByte(name)))
        {
            PrintMessage(name);
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
    std::vector<wsl::windows::common::wslc_schema::InspectNetwork> result;
    for (const auto& name : networkNames)
    {
        std::optional<wslc_schema::InspectNetwork> inspectData;
        if (TryInspectNetwork(session, WideToMultiByte(name), inspectData))
        {
            result.push_back(*inspectData);
        }
        else
        {
            context.ExitCode = 1;
        }
    }

    auto json = ToJson(result, c_jsonPrettyPrintIndent);
    PrintMessage(MultiByteToWide(json));
}

void ListNetworks(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Networks));
    auto& networks = context.Data.Get<Data::Networks>();

    if (context.Args.Contains(ArgType::Quiet))
    {
        for (const auto& network : networks)
        {
            PrintMessage(MultiByteToWide(network.Name));
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
        PrintMessage(MultiByteToWide(json));
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
