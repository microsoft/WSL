/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkTasks.cpp

Abstract:

    Implementation of network command related execution logic.

--*/
#include "Argument.h"
#include "ArgumentConvertedTypes.h"
#include "CLIExecutionContext.h"
#include "NetworkModel.h"
#include "NetworkService.h"
#include "NetworkTasks.h"
#include "TableOutput.h"
#include <wslc_schema.h>

using namespace wsl::shared;
using namespace wsl::windows::common;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::timestamp;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::models;
using namespace wsl::windows::wslc::services;

namespace wsl::windows::wslc::task {

namespace {

    // Shared by the table and json output so the two cannot drift. The id is truncated unless --no-trunc is passed.
    NetworkOutputInformation ToNetworkOutput(const wslc_schema::NetworkListEntry& network, bool truncate)
    {
        NetworkOutputInformation entry;
        entry.CreatedAt = Rfc3339ToUtcDisplayTime(network.Created);
        entry.Driver = network.Driver;
        entry.ID = TruncateId(network.Id, truncate);
        entry.IPv4 = network.EnableIPv4 ? "true" : "false";
        entry.IPv6 = network.EnableIPv6 ? "true" : "false";
        entry.Internal = network.Internal ? "true" : "false";
        entry.Name = network.Name;
        entry.Scope = network.Scope;

        for (const auto& [key, value] : network.Labels)
        {
            if (!entry.Labels.empty())
            {
                entry.Labels += ",";
            }

            entry.Labels += std::format("{}={}", key, value);
        }

        return entry;
    }

} // namespace

static bool TryInspectNetwork(Terminal& terminal, Session& session, const std::string& networkName, std::optional<wslc_schema::Network>& inspectData)
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
            terminal.Error(L"{}\n", Localization::MessageWslcNetworkNotFound(networkName.c_str()));
            return false;
        }

        throw;
    }
}

static bool TryDeleteNetwork(Terminal& terminal, Session& session, const std::string& networkName, bool force)
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
                terminal.Error(L"{}\n", Localization::MessageWslcNetworkNotFound(networkName.c_str()));
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
    options.Name = WideToMultiByte(context.Args.GetValue<ArgType::NetworkName>());

    for (const auto& option : context.Args.GetAllValues<ArgType::Options>())
    {
        options.DriverOpts.push_back(option);
    }

    for (const auto& label : context.Args.GetAllValues<ArgType::Label>())
    {
        options.Labels.push_back(label);
    }

    if (context.Args.Contains(ArgType::Driver))
    {
        options.Driver = WideToMultiByte(context.Args.GetValue<ArgType::Driver>());
    }

    options.Internal = context.Args.GetValue<ArgType::Internal>();

    if (context.Args.Contains(ArgType::Subnet))
    {
        options.Subnet = WideToMultiByte(context.Args.GetValue<ArgType::Subnet>());
    }

    if (context.Args.Contains(ArgType::Gateway))
    {
        options.Gateway = WideToMultiByte(context.Args.GetValue<ArgType::Gateway>());
    }

    if (context.Args.Contains(ArgType::IpRange))
    {
        options.IpRange = WideToMultiByte(context.Args.GetValue<ArgType::IpRange>());
    }

    NetworkService::Create(context.Terminal, context.Data.Get<Data::Session>(), options);
    context.Terminal.Output(L"{}\n", MultiByteToWide(options.Name));
}

void DeleteNetworks(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto networkNames = context.Args.GetAllValues<ArgType::NetworkName>();
    const bool force = context.Args.GetValue<ArgType::Force>();
    for (const auto& name : networkNames)
    {
        if (TryDeleteNetwork(context.Terminal, session, WideToMultiByte(name), force))
        {
            context.Terminal.Output(L"{}\n", name);
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

    auto filters = context.Args.GetAllValues<ArgType::Filter>();
    context.Data.Add<Data::Networks>(NetworkService::List(session, filters));
}

void InspectNetworks(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto networkNames = context.Args.GetAllValues<ArgType::NetworkName>();
    std::vector<wsl::windows::common::wslc_schema::Network> result;
    for (const auto& name : networkNames)
    {
        std::optional<wslc_schema::Network> inspectData;
        if (TryInspectNetwork(context.Terminal, session, WideToMultiByte(name), inspectData))
        {
            result.push_back(*inspectData);
        }
        else
        {
            context.ExitCode = 1;
        }
    }

    auto json = ToJson(result, context.Args.GetValue<ArgType::InspectFormat>(c_jsonPrettyPrintIndent));
    context.Terminal.Output(L"{}\n", MultiByteToWide(json));
}

void ListNetworks(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Networks));
    auto& networks = context.Data.Get<Data::Networks>();

    // Networks are reported in name order regardless of how the daemon returns them.
    std::ranges::sort(networks, {}, &wslc_schema::NetworkListEntry::Name);

    const auto format = context.Args.GetValue<ArgType::Format>(FormatType::Table);
    const bool quiet = context.Args.GetValue<ArgType::Quiet>();
    const bool trunc = !context.Args.GetValue<ArgType::NoTrunc>();
    if (format == FormatType::Table && quiet)
    {
        for (const auto& network : networks)
        {
            context.Terminal.Output(L"{}\n", MultiByteToWide(TruncateId(network.Id, trunc)));
        }

        return;
    }

    switch (format)
    {
    case FormatType::Json:
    {
        for (const auto& network : networks)
        {
            context.Terminal.Output(L"{}\n", ToJsonW(ToNetworkOutput(network, trunc), c_jsonCompactIndent));
        }

        break;
    }
    case FormatType::Table:
    {
        // Every column has a minimum total width of ten characters, including the padding that follows it.
        constexpr size_t c_minimumColumnWidth = 7;
        auto table = wsl::windows::wslc::TableOutput<4>(
            context.Terminal,
            {L"NETWORK ID", L"NAME", L"DRIVER", L"SCOPE"},
            {ColumnWidthConfig{.MinWidth = c_minimumColumnWidth},
             ColumnWidthConfig{.MinWidth = c_minimumColumnWidth},
             ColumnWidthConfig{.MinWidth = c_minimumColumnWidth},
             ColumnWidthConfig{.MinWidth = c_minimumColumnWidth}});
        for (const auto& network : networks)
        {
            const auto entry = ToNetworkOutput(network, trunc);
            table.WriteRow({
                MultiByteToWide(entry.ID),
                MultiByteToWide(entry.Name),
                MultiByteToWide(entry.Driver),
                MultiByteToWide(entry.Scope),
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

    // Filter values are parsed and cached during argument validation.
    auto filters = context.Args.GetAllValues<ArgType::Filter>();

    auto result = NetworkService::Prune(session, filters);

    if (result.PrunedNetworks.empty())
    {
        return;
    }

    context.Terminal.Output(L"{}\n", Localization::WSLCCLI_NetworkPruneDeletedHeader());
    for (const auto& networkName : result.PrunedNetworks)
    {
        context.Terminal.Output(L"{}\n", MultiByteToWide(networkName));
    }

    context.Terminal.Output(L"\n");
}

void ConnectNetwork(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Data.Contains(Data::NetworkEndpointOptions));
    WI_ASSERT(context.Args.Contains(ArgType::NetworkName));
    WI_ASSERT(context.Args.Contains(ArgType::ContainerId));

    const auto& endpoint = context.Data.Get<Data::NetworkEndpointOptions>();
    models::ConnectNetworkOptions options{};
    options.NetworkName = WideToMultiByte(context.Args.GetValue<ArgType::NetworkName>());
    options.ContainerId = WideToMultiByte(context.Args.GetValue<ArgType::ContainerId>());
    options.Aliases = endpoint.Aliases;
    options.IpAddress = endpoint.IpAddress;
    options.Links = endpoint.Links;
    options.LinkLocalIps = endpoint.LinkLocalIps;
    options.DriverOpts = endpoint.DriverOpts;

    NetworkService::Connect(context.Data.Get<Data::Session>(), options);
}

void DisconnectNetwork(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::NetworkName));
    WI_ASSERT(context.Args.Contains(ArgType::ContainerId));

    const auto networkName = WideToMultiByte(context.Args.GetValue<ArgType::NetworkName>());
    const auto containerId = WideToMultiByte(context.Args.GetValue<ArgType::ContainerId>());
    NetworkService::Disconnect(context.Data.Get<Data::Session>(), networkName, containerId);
}

void SetNetworkEndpointOptionsFromArgs(CLIExecutionContext& context)
{
    models::NetworkEndpointOptions options{};

    for (const auto& alias : context.Args.GetAllValues<ArgType::NetworkAlias>())
    {
        options.Aliases.emplace_back(WideToMultiByte(alias));
    }

    if (context.Args.Contains(ArgType::IpAddress))
    {
        options.IpAddress = WideToMultiByte(context.Args.GetValue<ArgType::IpAddress>());
    }

    for (const auto& link : context.Args.GetAllValues<ArgType::Link>())
    {
        options.Links.emplace_back(WideToMultiByte(link));
    }

    for (const auto& linkLocalIp : context.Args.GetAllValues<ArgType::LinkLocalIp>())
    {
        options.LinkLocalIps.emplace_back(WideToMultiByte(linkLocalIp));
    }

    for (const auto& driverOpt : context.Args.GetAllValues<ArgType::DriverOpt>())
    {
        options.DriverOpts.emplace_back(WideToMultiByte(driverOpt));
    }

    context.Data.Add<Data::NetworkEndpointOptions>(std::move(options));
}
} // namespace wsl::windows::wslc::task
