/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerTasks.cpp

Abstract:

    Implementation of container command related execution logic.

--*/
#include "Argument.h"
#include "ArgumentConvertedTypes.h"
#include "AsyncExecution.h"
#include "CLIExecutionContext.h"
#include "CommonTasks.h"
#include "ContainerModel.h"
#include "ContainerService.h"
#include "ContainerTasks.h"
#include "ImageModel.h"
#include "MountSpecParsing.h"
#include "SessionModel.h"
#include "SessionService.h"
#include "TableOutput.h"
#include <wil/result_macros.h>
#include <filesystem.hpp>
#include <wslc_schema.h>
#include <filesystem>

using namespace wsl::shared;
using namespace wsl::windows::common;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::timestamp;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::models;
using namespace wsl::windows::wslc::services;
using wsl::windows::common::string::FormatHumanReadableSize;
using wsl::windows::common::string::StorageSizeUnit;

namespace {

// Docker reports memory in binary units and network and block IO in decimal units.
constexpr uint32_t c_statsMemoryPrecision = 4;
constexpr uint32_t c_statsIoPrecision = 3;

std::string FormatStatsMemory(uint64_t Bytes)
{
    return WideToMultiByte(FormatHumanReadableSize(Bytes, c_statsMemoryPrecision, StorageSizeUnit::Binary));
}

std::string FormatStatsIo(uint64_t Bytes)
{
    return WideToMultiByte(FormatHumanReadableSize(Bytes, c_statsIoPrecision));
}

// The ps SIZE column: the writable layer on its own, and the total including the read-only image
// layers in parentheses. The suffix is guarded on 'SizeRootFs > 0', so a zero total renders as the
// writable size alone; the daemon reports zero both when the size was not requested and when there
// is no parent layer to measure. The table is localized while json keeps the invariant form.
std::wstring FormatContainerSize(LONGLONG SizeRw, LONGLONG SizeRootFs, FormatType format)
{
    const auto writable = FormatHumanReadableSize(static_cast<uint64_t>(std::max<LONGLONG>(SizeRw, 0)), c_statsIoPrecision);
    if (SizeRootFs <= 0)
    {
        return writable;
    }

    const auto total = FormatHumanReadableSize(static_cast<uint64_t>(SizeRootFs), c_statsIoPrecision);
    if (format == FormatType::Json)
    {
        return std::format(L"{} (virtual {})", writable, total);
    }

    return Localization::WSLCCLI_ContainerSizeWithVirtual(writable, total);
}

nlohmann::json ComputeContainerStatsJson(const wsl::windows::common::docker_schema::ContainerStats& stats)
{
    // Calculate CPU %
    // Formula matches Docker CLI: https://github.com/docker/cli/blob/master/cli/command/container/stats_helpers.go
    double cpuPercent = 0.0;
    const auto cpuDelta =
        static_cast<double>(stats.cpu_stats.cpu_usage.total_usage) - static_cast<double>(stats.precpu_stats.cpu_usage.total_usage);
    const auto systemDelta = static_cast<double>(stats.cpu_stats.system_cpu_usage) - static_cast<double>(stats.precpu_stats.system_cpu_usage);
    if (systemDelta > 0.0 && cpuDelta > 0.0)
    {
        uint32_t onlineCpus = stats.cpu_stats.online_cpus;
        if (onlineCpus == 0 && stats.cpu_stats.cpu_usage.percpu_usage.has_value())
        {
            onlineCpus = static_cast<uint32_t>(stats.cpu_stats.cpu_usage.percpu_usage->size());
        }

        cpuPercent = (cpuDelta / systemDelta) * static_cast<double>(onlineCpus) * 100.0;
    }

    // Calculate memory %
    double memPercent = 0.0;
    if (stats.memory_stats.limit > 0)
    {
        memPercent = (static_cast<double>(stats.memory_stats.usage) / static_cast<double>(stats.memory_stats.limit)) * 100.0;
    }

    // Aggregate network I/O
    uint64_t netRxBytes = 0;
    uint64_t netTxBytes = 0;
    if (stats.networks.has_value())
    {
        for (const auto& [iface, netStats] : *stats.networks)
        {
            netRxBytes += netStats.rx_bytes;
            netTxBytes += netStats.tx_bytes;
        }
    }

    // Aggregate block I/O
    uint64_t blkReadBytes = 0;
    uint64_t blkWriteBytes = 0;
    if (stats.blkio_stats.io_service_bytes_recursive.has_value())
    {
        for (const auto& entry : *stats.blkio_stats.io_service_bytes_recursive)
        {
            if (_stricmp(entry.op.c_str(), "read") == 0)
            {
                blkReadBytes += entry.value;
            }
            else if (_stricmp(entry.op.c_str(), "write") == 0)
            {
                blkWriteBytes += entry.value;
            }
        }
    }

    const auto& containerName = stats.name.empty() ? stats.id : stats.name;

    return {
        {"ID", stats.id},
        {"Name", containerName},
        {"CPUPerc", std::format("{:.2f}%", cpuPercent)},
        {"MemUsage", std::format("{} / {}", FormatStatsMemory(stats.memory_stats.usage), FormatStatsMemory(stats.memory_stats.limit))},
        {"MemPerc", std::format("{:.2f}%", memPercent)},
        {"NetIO", std::format("{} / {}", FormatStatsIo(netRxBytes), FormatStatsIo(netTxBytes))},
        {"BlockIO", std::format("{} / {}", FormatStatsIo(blkReadBytes), FormatStatsIo(blkWriteBytes))},
        {"PIDs", stats.pids_stats.current},
    };
}

// Builds the representation of a container, shared by the table and json output so the two cannot
// drift. Every value is emitted as a string apart from the platform object, and the id is truncated
// unless --no-trunc is passed. RunningFor, Size and Status are the only fields that vary with the
// format: docker renders them in invariant English, so json keeps that while the table is localized.
ContainerOutputInformation ToContainerOutput(const ContainerInformation& container, bool truncate, FormatType format)
{
    ContainerOutputInformation entry;
    entry.Command = WideToMultiByte(ContainerService::FormatCommand(container.Command, truncate));
    entry.CreatedAt = EpochToLocalDisplayTime(container.CreatedAt);
    // The runtime reports health as a suffix on the status description, which is the only place it is
    // exposed by the listing API.
    entry.HealthStatus = ContainerService::FormatHealthStatus(container.Status);
    entry.ID = truncate ? TruncateId(container.Id) : container.Id;
    entry.Image = container.Image;
    entry.Labels = container.Labels;
    entry.LocalVolumes = std::to_string(container.LocalVolumes);
    entry.Mounts = WideToMultiByte(ContainerService::FormatMounts(container.Mounts, truncate));
    entry.Names = container.Name;
    entry.Networks = container.Networks;
    entry.Platform.architecture = wsl::shared::Arm64 ? "arm64" : "amd64";
    entry.Platform.os = "linux";
    entry.Ports = WideToMultiByte(ContainerService::FormatPorts(container.State, container.Ports));
    entry.RunningFor = WideToMultiByte(
        format == FormatType::Json ? FormatInvariantRelativeTime(container.CreatedAt) : FormatRelativeTime(container.CreatedAt));
    // The daemon only computes container sizes when the listing request asks for them, so this is a
    // formatted zero unless --size was passed.
    entry.Size = WideToMultiByte(FormatContainerSize(container.SizeRw, container.SizeRootFs, format));
    entry.State = WideToMultiByte(ContainerService::ContainerStateName(container.State));
    entry.Status = WideToMultiByte(ContainerService::FormatStatus(container.Status, container.State, container.StateChangedAt, format));

    return entry;
}

} // namespace

namespace wsl::windows::wslc::task {

using namespace wsl::windows::wslc::cli;

// Every container is attempted even if an earlier one fails; the command still exits nonzero.
template <typename TAction>
static void ForEachContainer(CLIExecutionContext& context, TAction&& action)
{
    for (const auto& id : context.Args.GetAllValues<ArgType::ContainerId>())
    {
        try
        {
            action(WideToMultiByte(id));
            context.Terminal.Output(L"{}\n", id);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            context.ReportError(wil::ResultFromCaughtException());

            // CollectErrorImpl keeps the first message when the next container fails with the same HRESULT.
            context.ClearError();
            context.ExitCode = 1;
        }
    }
}

static bool TryInspectContainer(
    Terminal& terminal, Session& session, const std::string& containerId, std::optional<wslc_schema::InspectContainer>& inspectData, bool size = false)
{
    try
    {
        inspectData = ContainerService::Inspect(session, containerId, size);
        return true;
    }
    catch (const wil::ResultException& ex)
    {
        if (ex.GetErrorCode() == WSLC_E_CONTAINER_NOT_FOUND)
        {
            terminal.Error(L"{}\n", Localization::MessageWslcContainerNotFound(containerId.c_str()));
            return false;
        }

        throw;
    }
}

void AttachContainer::operator()(CLIExecutionContext& context) const
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    context.ExitCode = ContainerService::Attach(context.Terminal, context.Data.Get<Data::Session>(), WideToMultiByte(m_containerId));
}

void CreateContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    WI_ASSERT(context.Data.Contains(Data::ContainerOptions));
    auto result = ContainerService::Create(
        context.Terminal,
        context.Data.Get<Data::Session>(),
        WideToMultiByte(context.Args.GetValue<ArgType::ImageId>()),
        context.Data.Get<Data::ContainerOptions>());
    context.Terminal.Output(L"{}\n", MultiByteToWide(result.Id));
}

void ExecContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ContainerId));
    WI_ASSERT(context.Data.Contains(Data::ContainerOptions));
    context.ExitCode = ContainerService::Exec(
        context.Terminal,
        context.Data.Get<Data::Session>(),
        WideToMultiByte(context.Args.GetValue<ArgType::ContainerId>()),
        context.Data.Get<Data::ContainerOptions>());
}

void GetContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();

    int limit = -1;

    if (context.Args.Contains(ArgType::Last))
    {
        limit = context.Args.GetValue<ArgType::Last>();
    }
    else if (context.Args.GetValue<ArgType::Latest>())
    {
        limit = 1;
    }

    // Filter values are parsed and cached during argument validation.
    auto filters = context.Args.GetAllValues<ArgType::Filter>();

    // `container stats` reuses this task and does not register --size.
    const bool size = context.Args.Contains(ArgType::Size) && context.Args.GetValue<ArgType::Size>();

    context.Data.Add<Data::Containers>(ContainerService::List(session, context.Args.GetValue<ArgType::All>(), limit, filters, size));
}

void InspectContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containerIds = context.Args.GetAllValues<ArgType::ContainerId>();
    std::vector<wsl::windows::common::wslc_schema::InspectContainer> result;
    const bool size = context.Args.GetValue<ArgType::Size>();
    for (const auto& id : containerIds)
    {
        std::optional<wslc_schema::InspectContainer> inspectData;
        if (TryInspectContainer(context.Terminal, session, WideToMultiByte(id), inspectData, size))
        {
            result.push_back(*inspectData);
        }
        else
        {
            context.ExitCode = 1;
        }
    }

    nlohmann::json array = nlohmann::json::array();
    for (const auto& entry : result)
    {
        array.push_back(wslc_schema::ToInspectJson(entry));
    }

    auto json = array.dump(context.Args.GetValue<ArgType::InspectFormat>(c_jsonPrettyPrintIndent));
    context.Terminal.Output(L"{}\n", MultiByteToWide(json));
}

void KillContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    const auto signal = context.Args.GetValue<ArgType::Signal>(WSLCSignalSIGKILL);

    ForEachContainer(context, [&](const std::string& id) { ContainerService::Kill(session, id, signal); });
}

void ExportContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ContainerId));
    auto& session = context.Data.Get<Data::Session>();
    auto containerId = WideToMultiByte(context.Args.GetValue<ArgType::ContainerId>());

    if (context.Args.Contains(ArgType::Output))
    {
        auto& output = context.Args.GetValue<ArgType::Output>();
        ContainerService::Export(session, containerId, output);
    }
    else
    {
        auto stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (wsl::windows::common::wslutil::IsConsoleHandle(stdoutHandle))
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::WSLCCLI_ContainerExportStdoutIsTerminalError());
        }

        ContainerService::Export(session, containerId, stdoutHandle);
    }
}

void ContainerCp(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::Source));
    WI_ASSERT(context.Args.Contains(ArgType::Target));

    auto& session = context.Data.Get<Data::Session>();
    const auto& source = context.Args.GetValue<ArgType::Source>();
    const auto& target = context.Args.GetValue<ArgType::Target>();
    const bool followLink = context.Args.GetValue<ArgType::FollowLink>();

    ContainerService::Copy(session, source, target, followLink);
}

void ListContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Containers));
    auto& containers = context.Data.Get<Data::Containers>();

    // Note: --all and --filter status= are honored by the Docker daemon when
    // GetContainers ran; no post-filtering needed here.

    if (context.Args.GetValue<ArgType::Quiet>())
    {
        // Print only the container ids
        bool trunc = !context.Args.GetValue<ArgType::NoTrunc>();
        for (const auto& container : containers)
        {
            context.Terminal.Output(L"{}\n", MultiByteToWide(trunc ? TruncateId(container.Id) : container.Id));
        }

        return;
    }

    const auto format = context.Args.GetValue<ArgType::Format>(FormatType::Table);
    bool trunc = !context.Args.GetValue<ArgType::NoTrunc>();

    switch (format)
    {
    case FormatType::Json:
    {
        for (const auto& container : containers)
        {
            context.Terminal.Output(L"{}\n", ToJsonW(ToContainerOutput(container, trunc, FormatType::Json), c_jsonCompactIndent));
        }

        break;
    }
    case FormatType::Table:
    {
        using enum ColumnOverflow;

        // SIZE trails the other columns. It is always declared, and is left empty and hidden unless
        // --size was passed.
        constexpr size_t c_sizeColumn = 7;
        const bool showSize = context.Args.GetValue<ArgType::Size>();

        // Create table with or without column limits based on --no-trunc flag
        auto table = trunc ? wsl::windows::wslc::cli::TableOutput<8>(
                                 context.Terminal,
                                 {{{Localization::WSLCCLI_TableHeaderContainerId(), {.MaxWidth = 12, .Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderImage(), {.MaxWidth = 20, .Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderCommand(), {.Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderCreated(), {.Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderStatus(), {.Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderPorts(), {.Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderNames(), {.MaxWidth = 20, .Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderSize(), {.Overflow = Shrink}}}},
                                 containers.size())
                           : wsl::windows::wslc::cli::TableOutput<8>(
                                 context.Terminal,
                                 {Localization::WSLCCLI_TableHeaderContainerId(),
                                  Localization::WSLCCLI_TableHeaderImage(),
                                  Localization::WSLCCLI_TableHeaderCommand(),
                                  Localization::WSLCCLI_TableHeaderCreated(),
                                  Localization::WSLCCLI_TableHeaderStatus(),
                                  Localization::WSLCCLI_TableHeaderPorts(),
                                  Localization::WSLCCLI_TableHeaderNames(),
                                  Localization::WSLCCLI_TableHeaderSize()});

        table.SetColumnHidden(c_sizeColumn, !showSize);

        for (const auto& container : containers)
        {
            const auto entry = ToContainerOutput(container, trunc, FormatType::Table);
            table.WriteRow({
                MultiByteToWide(entry.ID),
                MultiByteToWide(entry.Image),
                MultiByteToWide(entry.Command),
                MultiByteToWide(entry.RunningFor),
                MultiByteToWide(entry.Status),
                MultiByteToWide(entry.Ports),
                MultiByteToWide(entry.Names),
                showSize ? MultiByteToWide(entry.Size) : std::wstring{},
            });
        }

        table.Complete();

        break;
    }
    default:
        THROW_HR(E_UNEXPECTED);
    }
}

void RemoveContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    const bool force = context.Args.GetValue<ArgType::Force>();
    const bool deleteVolumes = context.Args.GetValue<ArgType::Volumes>();

    ForEachContainer(context, [&](const std::string& id) { ContainerService::Delete(session, id, force, deleteVolumes); });
}

void RunContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    WI_ASSERT(context.Data.Contains(Data::ContainerOptions));
    context.ExitCode = ContainerService::Run(
        context.Terminal,
        context.Data.Get<Data::Session>(),
        WideToMultiByte(context.Args.GetValue<ArgType::ImageId>()),
        context.Data.Get<Data::ContainerOptions>());
}

void SetContainerOptionsFromArgs(CLIExecutionContext& context)
{
    ContainerOptions options;

    if (context.Args.Contains(ArgType::Pull))
    {
        options.Pull = context.Args.GetValue<ArgType::Pull>();
    }

    if (context.Args.Contains(ArgType::CIDFile))
    {
        options.CidFile = context.Args.GetValue<ArgType::CIDFile>();
    }

    if (context.Args.Contains(ArgType::Name))
    {
        options.Name = WideToMultiByte(context.Args.GetValue<ArgType::Name>());
    }

    options.TTY = context.Args.GetValue<ArgType::TTY>();
    options.Detach = context.Args.GetValue<ArgType::Detach>();
    options.Interactive = context.Args.GetValue<ArgType::Interactive>();

    if (context.Args.Contains(ArgType::Publish))
    {
        auto ports = context.Args.GetAllValues<ArgType::Publish>();
        options.Ports.reserve(options.Ports.size() + ports.size());
        for (const auto& port : ports)
        {
            options.Ports.emplace_back(WideToMultiByte(port));
        }
    }

    options.PublishAll = context.Args.GetValue<ArgType::PublishAll>();

    if (context.Args.Contains(ArgType::Gpus))
    {
        options.Gpu = true;
    }

    if (context.Args.Contains(ArgType::Volume))
    {
        auto volumes = context.Args.GetAllValues<ArgType::Volume>();
        options.Mounts.insert(options.Mounts.end(), std::make_move_iterator(volumes.begin()), std::make_move_iterator(volumes.end()));
    }

    if (context.Args.Contains(ArgType::Mount))
    {
        auto mounts = context.Args.GetAllValues<ArgType::Mount>();
        options.Mounts.insert(options.Mounts.end(), std::make_move_iterator(mounts.begin()), std::make_move_iterator(mounts.end()));
    }

    options.Remove = context.Args.GetValue<ArgType::Remove>();

    if (context.Args.Contains(ArgType::StopSignal))
    {
        options.StopSignal = context.Args.GetValue<ArgType::StopSignal>();
    }

    if (context.Args.Contains(ArgType::StopTimeout))
    {
        options.StopTimeout = context.Args.GetValue<ArgType::StopTimeout>();
    }

    if (context.Args.Contains(ArgType::ShmSize))
    {
        options.ShmSize = context.Args.GetValue<ArgType::ShmSize>();
    }

    if (context.Args.Contains(ArgType::HealthCmd))
    {
        options.HealthCmd = WideToMultiByte(context.Args.GetValue<ArgType::HealthCmd>());
    }

    if (context.Args.Contains(ArgType::HealthInterval))
    {
        options.HealthInterval = context.Args.GetValue<ArgType::HealthInterval>();
    }

    if (context.Args.Contains(ArgType::HealthTimeout))
    {
        options.HealthTimeout = context.Args.GetValue<ArgType::HealthTimeout>();
    }

    if (context.Args.Contains(ArgType::HealthStartPeriod))
    {
        options.HealthStartPeriod = context.Args.GetValue<ArgType::HealthStartPeriod>();
    }

    if (context.Args.Contains(ArgType::HealthRetries))
    {
        options.HealthRetries = context.Args.GetValue<ArgType::HealthRetries>();
    }

    options.NoHealthcheck = context.Args.GetValue<ArgType::NoHealthcheck>();

    if (context.Args.Contains(ArgType::Memory))
    {
        options.MemoryBytes = context.Args.GetValue<ArgType::Memory>();
    }

    if (context.Args.Contains(ArgType::Cpus))
    {
        options.NanoCpus = context.Args.GetValue<ArgType::Cpus>();
    }

    options.Ulimits = context.Args.GetAllValues<ArgType::Ulimit>();

    if (context.Args.Contains(ArgType::Command))
    {
        options.Arguments.emplace_back(WideToMultiByte(context.Args.GetValue<ArgType::Command>()));
    }

    if (context.Args.Contains(ArgType::EnvFile))
    {
        auto envFiles = context.Args.GetAllValues<ArgType::EnvFile>();
        for (const auto& envFile : envFiles)
        {
            auto parsedEnvVars = EnvironmentVariable::ParseFile(envFile);
            for (const auto& envVar : parsedEnvVars)
            {
                options.EnvironmentVariables.push_back(wsl::shared::string::WideToMultiByte(envVar));
            }
        }
    }

    if (context.Args.Contains(ArgType::Env))
    {
        auto envArgs = context.Args.GetAllValues<ArgType::Env>();
        for (const auto& arg : envArgs)
        {
            auto envVar = EnvironmentVariable::Parse(arg);
            if (envVar)
            {
                options.EnvironmentVariables.push_back(wsl::shared::string::WideToMultiByte(*envVar));
            }
        }
    }

    if (context.Args.Contains(ArgType::Entrypoint))
    {
        options.Entrypoint.push_back(WideToMultiByte(context.Args.GetValue<ArgType::Entrypoint>()));
    }

    if (context.Args.Contains(ArgType::Hostname))
    {
        options.Hostname = WideToMultiByte(context.Args.GetValue<ArgType::Hostname>());
    }

    if (context.Args.Contains(ArgType::Domainname))
    {
        options.Domainname = WideToMultiByte(context.Args.GetValue<ArgType::Domainname>());
    }

    if (context.Args.Contains(ArgType::DNS))
    {
        auto dnsServers = context.Args.GetAllValues<ArgType::DNS>();
        options.DnsServers.reserve(options.DnsServers.size() + dnsServers.size());
        for (const auto& value : dnsServers)
        {
            options.DnsServers.emplace_back(WideToMultiByte(value));
        }
    }

    if (context.Args.Contains(ArgType::DNSSearch))
    {
        auto dnsSearch = context.Args.GetAllValues<ArgType::DNSSearch>();
        options.DnsSearchDomains.reserve(options.DnsSearchDomains.size() + dnsSearch.size());
        for (const auto& value : dnsSearch)
        {
            options.DnsSearchDomains.emplace_back(WideToMultiByte(value));
        }
    }

    if (context.Args.Contains(ArgType::DNSOption))
    {
        auto dnsOptions = context.Args.GetAllValues<ArgType::DNSOption>();
        options.DnsOptions.reserve(options.DnsOptions.size() + dnsOptions.size());
        for (const auto& value : dnsOptions)
        {
            options.DnsOptions.emplace_back(WideToMultiByte(value));
        }
    }

    if (context.Args.Contains(ArgType::Network))
    {
        auto networks = context.Args.GetAllValues<ArgType::Network>();
        options.Networks.reserve(options.Networks.size() + networks.size());
        for (auto& parsed : networks)
        {
            auto& network = options.Networks.emplace_back();
            network.Name = std::move(parsed.Name);
            network.Aliases = std::move(parsed.Aliases);
        }
    }

    if (context.Args.Contains(ArgType::NetworkAlias))
    {
        auto aliases = context.Args.GetAllValues<ArgType::NetworkAlias>();
        options.NetworkAliases.reserve(aliases.size());
        for (const auto& value : aliases)
        {
            options.NetworkAliases.emplace_back(WideToMultiByte(value));
        }
    }

    if (context.Args.Contains(ArgType::IpAddress))
    {
        options.IpAddress = WideToMultiByte(context.Args.GetValue<ArgType::IpAddress>());
    }

    if (context.Args.Contains(ArgType::User))
    {
        options.User = WideToMultiByte(context.Args.GetValue<ArgType::User>());
    }

    if (context.Args.Contains(ArgType::TMPFS))
    {
        auto tmpfs = context.Args.GetAllValues<ArgType::TMPFS>();
        options.Mounts.insert(options.Mounts.end(), std::make_move_iterator(tmpfs.begin()), std::make_move_iterator(tmpfs.end()));
    }

    for (const auto& label : context.Args.GetAllValues<ArgType::Label>())
    {
        options.Labels.push_back(label);
    }

    if (context.Args.Contains(ArgType::ForwardArgs))
    {
        auto const& forwardArgs = context.Args.GetValue<ArgType::ForwardArgs>();
        options.Arguments.reserve(options.Arguments.size() + forwardArgs.size());
        for (const auto& arg : forwardArgs)
        {
            options.Arguments.emplace_back(WideToMultiByte(arg));
        }
    }

    if (context.Args.Contains(ArgType::WorkDir))
    {
        options.WorkingDirectory = WideToMultiByte(context.Args.GetValue<ArgType::WorkDir>());
    }

    context.Data.Add<Data::ContainerOptions>(std::move(options));
}

void ShowContainerStats(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();

    auto containers = context.Args.GetAllValues<ArgType::ContainerId>();

    // If any are specified we use those, otherwise we show all containers.
    const bool userSpecifiedContainers = !containers.empty();
    if (!userSpecifiedContainers)
    {
        GetContainers(context);
        const auto& allContainers = context.Data.Get<Data::Containers>();
        for (const auto& container : allContainers)
        {
            // Skip non-running containers unless --all is specified.
            if (!context.Args.GetValue<ArgType::All>() && container.State != WSLCContainerState::WslcContainerStateRunning)
            {
                continue;
            }

            containers.push_back(MultiByteToWide(container.Id));
        }
    }

    // Fetch stats for all containers concurrently in batches. The Docker engine blocks for ~1s
    // per request to collect a valid precpu_stats sample, so issuing requests in parallel keeps
    // wall time proportional to ceil(N / batchSize) rather than N.
    nlohmann::json statsJson = nlohmann::json::array();
    wsl::windows::wslc::ForEachAsync<std::wstring>(
        containers,
        // Work to be done for each container ID on a separate thread.
        [&session](const std::wstring& containerId) {
            // ContainerService::Stats makes COM calls, so we must ensure COM is initialized on this thread.
            auto comCleanup = wil::CoInitializeEx(COINIT_MULTITHREADED);
            return ComputeContainerStatsJson(ContainerService::Stats(session, WideToMultiByte(containerId)));
        },
        // On Success
        [&](const nlohmann::json& entry) { statsJson.push_back(entry); },
        // On Error
        [&](const std::wstring& containerId, wil::ResultException error) {
            if (!userSpecifiedContainers)
            {
                switch (error.GetErrorCode())
                {
                case RPC_E_DISCONNECTED:
                case WSLC_E_CONTAINER_NOT_FOUND:
                    // Container disappeared between list and stats fetch, and
                    // the user did not specify these containers, so silently skip.
                    return;
                }
            }

            // Failure to retrieve a container should stop execution with
            // no container information displayed.
            LOG_HR_MSG(error.GetErrorCode(), "Failed to get stats for container %ws", containerId.c_str());
            throw error;
        },
        10 // Batch Size - chosen to be around typical expected container use while protecting against extreme cases.
    );

    const auto format = context.Args.GetValue<ArgType::Format>(FormatType::Table);

    switch (format)
    {
    case FormatType::Json:
    {
        for (const auto& entry : statsJson)
        {
            context.Terminal.Output(L"{}\n", ToJsonW(entry, c_jsonCompactIndent));
        }

        break;
    }
    case FormatType::Table:
    {
        bool trunc = !context.Args.GetValue<ArgType::NoTrunc>();
        using enum ColumnOverflow;

        auto table = trunc ? wsl::windows::wslc::cli::TableOutput<8>(
                                 context.Terminal,
                                 {{{Localization::WSLCCLI_TableHeaderContainerId(), {.MaxWidth = 12, .Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderName(), {.MaxWidth = 20, .Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderCpuPercent(), {.Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderMemUsageLimit(), {.Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderMemPercent(), {.Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderNetIo(), {.Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderBlockIo(), {.Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderPids(), {.Overflow = Shrink}}}},
                                 statsJson.size())
                           : wsl::windows::wslc::cli::TableOutput<8>(
                                 context.Terminal,
                                 {Localization::WSLCCLI_TableHeaderContainerId(),
                                  Localization::WSLCCLI_TableHeaderName(),
                                  Localization::WSLCCLI_TableHeaderCpuPercent(),
                                  Localization::WSLCCLI_TableHeaderMemUsageLimit(),
                                  Localization::WSLCCLI_TableHeaderMemPercent(),
                                  Localization::WSLCCLI_TableHeaderNetIo(),
                                  Localization::WSLCCLI_TableHeaderBlockIo(),
                                  Localization::WSLCCLI_TableHeaderPids()});

        for (const auto& entry : statsJson)
        {
            const auto id = entry["ID"].get<std::string>();
            table.WriteRow({
                MultiByteToWide(trunc ? TruncateId(id) : id),
                MultiByteToWide(entry["Name"].get<std::string>()),
                MultiByteToWide(entry["CPUPerc"].get<std::string>()),
                MultiByteToWide(entry["MemUsage"].get<std::string>()),
                MultiByteToWide(entry["MemPerc"].get<std::string>()),
                MultiByteToWide(entry["NetIO"].get<std::string>()),
                MultiByteToWide(entry["BlockIO"].get<std::string>()),
                std::to_wstring(entry["PIDs"].get<uint64_t>()),
            });
        }

        table.Complete();
        break;
    }
    default:
        THROW_HR(E_UNEXPECTED);
    }
}

void StartContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ContainerId));
    const auto& containerId = context.Args.GetValue<ArgType::ContainerId>();
    const bool attach = context.Args.GetValue<ArgType::Attach>();
    context.ExitCode = ContainerService::Start(context.Terminal, context.Data.Get<Data::Session>(), WideToMultiByte(containerId), attach);

    if (!attach)
    {
        context.Terminal.Output(L"{}\n", containerId);
    }
}

void StopContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    StopContainerOptions options;

    // WSLCSignalNone lets Docker use the container's configured STOPSIGNAL, or its default when none is configured.
    options.Signal = context.Args.GetValue<ArgType::Signal>(WSLCSignalNone);

    if (context.Args.Contains(ArgType::Time))
    {
        options.Timeout = context.Args.GetValue<ArgType::Time>();
    }

    ForEachContainer(context, [&](const std::string& id) { ContainerService::Stop(session, id, options); });
}

void RestartContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    StopContainerOptions options;

    // WSLCSignalNone lets Docker use the container's configured STOPSIGNAL, or its default when none is configured.
    options.Signal = context.Args.GetValue<ArgType::Signal>(WSLCSignalNone);

    if (context.Args.Contains(ArgType::Timeout))
    {
        options.Timeout = context.Args.GetValue<ArgType::Timeout>();
    }

    ForEachContainer(context, [&](const std::string& id) { ContainerService::Restart(context.Terminal, session, id, options); });
}

void ViewContainerLogs(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containerId = context.Args.GetValue<ArgType::ContainerId>();
    bool follow = context.Args.GetValue<ArgType::Follow>();
    bool timestamps = context.Args.GetValue<ArgType::Timestamps>();
    bool details = context.Args.GetValue<ArgType::Details>();

    ULONGLONG tail = 0;
    if (context.Args.Contains(ArgType::Tail))
    {
        tail = context.Args.GetValue<ArgType::Tail>();
    }

    // N.B. since=0 and until=0 mean "unset" — the Docker API omits the parameter when the value is 0,
    // which is equivalent to "no lower/upper bound". This matches Docker CLI behavior where
    // `docker logs --since 0` returns all logs and `docker logs --until 0` applies no upper bound.
    LONGLONG since = 0;
    if (context.Args.Contains(ArgType::Since))
    {
        since = context.Args.GetValue<ArgType::Since>();
    }

    LONGLONG until = 0;
    if (context.Args.Contains(ArgType::Until))
    {
        until = context.Args.GetValue<ArgType::Until>();
    }

    ContainerService::Logs(session, WideToMultiByte(containerId), follow, timestamps, details, since, until, tail);
}

void PruneContainers(CLIExecutionContext& context)
{
    context.Data.Add<Data::ConfirmWarning>(Localization::WSLCCLI_ContainerPruneConfirm());
    context.Data.Add<Data::ConfirmMessage>(Localization::WSLCCLI_PruneConfirmPrompt());
    ConfirmAction(context);

    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();

    // Filter values are parsed and cached during argument validation.
    auto filters = context.Args.GetAllValues<ArgType::Filter>();

    auto result = ContainerService::Prune(session, filters);

    if (!result.PrunedContainers.empty())
    {
        context.Terminal.Output(L"{}\n", Localization::WSLCCLI_ContainerPruneDeletedHeader());
        for (const auto& containerId : result.PrunedContainers)
        {
            context.Terminal.Output(L"{}\n", MultiByteToWide(containerId));
        }

        context.Terminal.Output(L"\n");
    }

    context.Terminal.Output(
        L"{}\n", Localization::WSLCCLI_ContainerPruneSpaceReclaimedBytes(FormatHumanReadableSize(result.SpaceReclaimed, c_reclaimedSpacePrecision)));
}
} // namespace wsl::windows::wslc::task
