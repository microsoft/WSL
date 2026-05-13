/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerTasks.cpp

Abstract:

    Implementation of container command related execution logic.

--*/
#include "Argument.h"
#include "ArgumentValidation.h"
#include "CLIExecutionContext.h"
#include "ContainerModel.h"
#include "ContainerService.h"
#include "ContainerTasks.h"
#include "SessionModel.h"
#include "SessionService.h"
#include "TableOutput.h"
#include "VolumeModel.h"
#include <wil/result_macros.h>
#include <wslc_schema.h>

using namespace wsl::shared;
using namespace wsl::windows::common;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::models;
using namespace wsl::windows::wslc::services;

namespace {

std::string FormatBytes(uint64_t bytes)
{
    constexpr uint64_t c_kib = 1024;
    constexpr uint64_t c_mib = 1024 * c_kib;
    constexpr uint64_t c_gib = 1024 * c_mib;

    if (bytes >= c_gib)
    {
        return std::format("{:.2f} GiB", static_cast<double>(bytes) / static_cast<double>(c_gib));
    }
    else if (bytes >= c_mib)
    {
        return std::format("{:.2f} MiB", static_cast<double>(bytes) / static_cast<double>(c_mib));
    }
    else if (bytes >= c_kib)
    {
        return std::format("{:.2f} KiB", static_cast<double>(bytes) / static_cast<double>(c_kib));
    }
    else
    {
        // Bytes are always whole numbers, so decimal places are intentionally omitted here.
        // This matches the behaviour of `docker stats`.
        return std::format("{} B", bytes);
    }
}

} // namespace

namespace wsl::windows::wslc::task {

static bool TryInspectContainer(Session& session, const std::string& containerId, std::optional<wslc_schema::InspectContainer>& inspectData)
{
    try
    {
        inspectData = ContainerService::Inspect(session, containerId);
        return true;
    }
    catch (const wil::ResultException& ex)
    {
        if (ex.GetErrorCode() == WSLC_E_CONTAINER_NOT_FOUND)
        {
            PrintMessage(Localization::MessageWslcContainerNotFound(containerId.c_str()), stderr);
            return false;
        }

        throw;
    }
}

void AttachContainer::operator()(CLIExecutionContext& context) const
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    context.ExitCode = ContainerService::Attach(context.Data.Get<Data::Session>(), WideToMultiByte(m_containerId));
}

void CreateContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    WI_ASSERT(context.Data.Contains(Data::ContainerOptions));
    auto result = ContainerService::Create(
        context.Data.Get<Data::Session>(), WideToMultiByte(context.Args.Get<ArgType::ImageId>()), context.Data.Get<Data::ContainerOptions>());
    PrintMessage(MultiByteToWide(result.Id));
}

void ExecContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ContainerId));
    WI_ASSERT(context.Data.Contains(Data::ContainerOptions));
    context.ExitCode = ContainerService::Exec(
        context.Data.Get<Data::Session>(), WideToMultiByte(context.Args.Get<ArgType::ContainerId>()), context.Data.Get<Data::ContainerOptions>());
}

void GetContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    context.Data.Add<Data::Containers>(ContainerService::List(session));
}

void InspectContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containerIds = context.Args.GetAll<ArgType::ContainerId>();
    std::vector<wsl::windows::common::wslc_schema::InspectContainer> result;
    for (const auto& id : containerIds)
    {
        std::optional<wslc_schema::InspectContainer> inspectData;
        if (TryInspectContainer(session, WideToMultiByte(id), inspectData))
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

void KillContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containerIds = context.Args.GetAll<ArgType::ContainerId>();
    WSLCSignal signal = WSLCSignalSIGKILL;
    if (context.Args.Contains(ArgType::Signal))
    {
        signal = validation::GetWSLCSignalFromString(context.Args.Get<ArgType::Signal>());
    }

    for (const auto& id : containerIds)
    {
        ContainerService::Kill(session, WideToMultiByte(id), signal);
    }
}

void ListContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Containers));
    auto& containers = context.Data.Get<Data::Containers>();

    // Filter by running state if --all is not specified
    if (!context.Args.Contains(ArgType::All))
    {
        auto shouldRemove = [](const ContainerInformation& container) {
            return container.State != WSLCContainerState::WslcContainerStateRunning;
        };
        containers.erase(std::remove_if(containers.begin(), containers.end(), shouldRemove), containers.end());
    }

    if (context.Args.Contains(ArgType::Quiet))
    {
        // Print only the container ids
        for (const auto& container : containers)
        {
            PrintMessage(MultiByteToWide(container.Id));
        }

        return;
    }

    FormatType format = FormatType::Table; // Default is table
    if (context.Args.Contains(ArgType::Format))
    {
        format = validation::GetFormatTypeFromString(context.Args.Get<ArgType::Format>());
    }

    switch (format)
    {
    case FormatType::Json:
    {
        auto json = ToJson(containers, c_jsonPrettyPrintIndent);
        PrintMessage(MultiByteToWide(json));
        break;
    }
    case FormatType::Table:
    {
        using Config = wsl::windows::wslc::ColumnWidthConfig;
        bool trunc = !context.Args.Contains(ArgType::NoTrunc);

        // Create table with or without column limits based on --no-trunc flag
        auto table = trunc ? wsl::windows::wslc::TableOutput<6>(
                                 {{{Localization::WSLCCLI_TableHeaderContainerId(), {Config::NoLimit, 12, false}},
                                   {Localization::WSLCCLI_TableHeaderName(), {Config::NoLimit, 20, true}},
                                   {Localization::WSLCCLI_TableHeaderImage(), {Config::NoLimit, 20, false}},
                                   {Localization::WSLCCLI_TableHeaderCreated(), {Config::NoLimit, Config::NoLimit, false}},
                                   {Localization::WSLCCLI_TableHeaderStatus(), {Config::NoLimit, Config::NoLimit, false}},
                                   {Localization::WSLCCLI_TableHeaderPorts(), {Config::NoLimit, Config::NoLimit, false}}}})
                           : wsl::windows::wslc::TableOutput<6>(
                                 {Localization::WSLCCLI_TableHeaderContainerId(),
                                  Localization::WSLCCLI_TableHeaderName(),
                                  Localization::WSLCCLI_TableHeaderImage(),
                                  Localization::WSLCCLI_TableHeaderCreated(),
                                  Localization::WSLCCLI_TableHeaderStatus(),
                                  Localization::WSLCCLI_TableHeaderPorts()});

        // Add each container as a row
        for (const auto& container : containers)
        {
            table.OutputLine({
                MultiByteToWide(trunc ? TruncateId(container.Id) : container.Id),
                MultiByteToWide(container.Name),
                MultiByteToWide(container.Image),
                ContainerService::FormatRelativeTime(container.CreatedAt),
                ContainerService::ContainerStateToString(container.State, container.StateChangedAt),
                ContainerService::FormatPorts(container.State, container.Ports),
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
    auto containerIds = context.Args.GetAll<ArgType::ContainerId>();
    bool force = context.Args.Contains(ArgType::Force);
    for (const auto& id : containerIds)
    {
        ContainerService::Delete(session, WideToMultiByte(id), force);
    }
}

void RunContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    WI_ASSERT(context.Data.Contains(Data::ContainerOptions));
    context.ExitCode = ContainerService::Run(
        context.Data.Get<Data::Session>(), WideToMultiByte(context.Args.Get<ArgType::ImageId>()), context.Data.Get<Data::ContainerOptions>());
}

void SetContainerOptionsFromArgs(CLIExecutionContext& context)
{
    ContainerOptions options;

    if (context.Args.Contains(ArgType::CIDFile))
    {
        options.CidFile = context.Args.Get<ArgType::CIDFile>();
    }

    if (context.Args.Contains(ArgType::Name))
    {
        options.Name = WideToMultiByte(context.Args.Get<ArgType::Name>());
    }

    if (context.Args.Contains(ArgType::TTY))
    {
        options.TTY = true;
    }

    if (context.Args.Contains(ArgType::Detach))
    {
        options.Detach = true;
    }

    if (context.Args.Contains(ArgType::Interactive))
    {
        options.Interactive = true;
    }

    if (context.Args.Contains(ArgType::Publish))
    {
        auto ports = context.Args.GetAll<ArgType::Publish>();
        options.Ports.reserve(options.Ports.size() + ports.size());
        for (const auto& port : ports)
        {
            options.Ports.emplace_back(WideToMultiByte(port));
        }
    }

    if (context.Args.Contains(ArgType::PublishAll))
    {
        options.PublishAll = true;
    }

    if (context.Args.Contains(ArgType::Gpus))
    {
        options.Gpu = true;
    }

    if (context.Args.Contains(ArgType::Volume))
    {
        auto volumes = context.Args.GetAll<ArgType::Volume>();
        options.Volumes.reserve(options.Volumes.size() + volumes.size());
        for (const auto& volume : volumes)
        {
            options.Volumes.emplace_back(volume);
        }
    }

    if (context.Args.Contains(ArgType::Remove))
    {
        options.Remove = true;
    }

    if (context.Args.Contains(ArgType::StopSignal))
    {
        options.StopSignal = validation::GetWSLCSignalFromString(context.Args.Get<ArgType::StopSignal>());
    }

    if (context.Args.Contains(ArgType::ShmSize))
    {
        options.ShmSize = validation::GetMemorySizeFromString(context.Args.Get<ArgType::ShmSize>());
    }

    if (context.Args.Contains(ArgType::Command))
    {
        options.Arguments.emplace_back(WideToMultiByte(context.Args.Get<ArgType::Command>()));
    }

    if (context.Args.Contains(ArgType::EnvFile))
    {
        auto const& envFiles = context.Args.GetAll<ArgType::EnvFile>();
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
        auto const& envArgs = context.Args.GetAll<ArgType::Env>();
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
        options.Entrypoint.push_back(WideToMultiByte(context.Args.Get<ArgType::Entrypoint>()));
    }

    if (context.Args.Contains(ArgType::Hostname))
    {
        options.Hostname = WideToMultiByte(context.Args.Get<ArgType::Hostname>());
    }

    if (context.Args.Contains(ArgType::Domainname))
    {
        options.Domainname = WideToMultiByte(context.Args.Get<ArgType::Domainname>());
    }

    if (context.Args.Contains(ArgType::DNS))
    {
        auto dnsServers = context.Args.GetAll<ArgType::DNS>();
        options.DnsServers.reserve(options.DnsServers.size() + dnsServers.size());
        for (const auto& value : dnsServers)
        {
            options.DnsServers.emplace_back(WideToMultiByte(value));
        }
    }

    if (context.Args.Contains(ArgType::DNSSearch))
    {
        auto dnsSearch = context.Args.GetAll<ArgType::DNSSearch>();
        options.DnsSearchDomains.reserve(options.DnsSearchDomains.size() + dnsSearch.size());
        for (const auto& value : dnsSearch)
        {
            options.DnsSearchDomains.emplace_back(WideToMultiByte(value));
        }
    }

    if (context.Args.Contains(ArgType::DNSOption))
    {
        auto dnsOptions = context.Args.GetAll<ArgType::DNSOption>();
        options.DnsOptions.reserve(options.DnsOptions.size() + dnsOptions.size());
        for (const auto& value : dnsOptions)
        {
            options.DnsOptions.emplace_back(WideToMultiByte(value));
        }
    }

    if (context.Args.Contains(ArgType::User))
    {
        options.User = WideToMultiByte(context.Args.Get<ArgType::User>());
    }

    if (context.Args.Contains(ArgType::TMPFS))
    {
        auto tmpfs = context.Args.GetAll<ArgType::TMPFS>();
        options.Tmpfs.reserve(options.Tmpfs.size() + tmpfs.size());
        for (const auto& value : tmpfs)
        {
            options.Tmpfs.emplace_back(WideToMultiByte(value));
        }
    }

    if (context.Args.Contains(ArgType::Label))
    {
        for (const auto& label : context.Args.GetAll<ArgType::Label>())
        {
            auto parsed = Label::Parse(label);
            options.Labels.emplace_back(parsed.first, parsed.second);
        }
    }

    if (context.Args.Contains(ArgType::ForwardArgs))
    {
        auto const& forwardArgs = context.Args.Get<ArgType::ForwardArgs>();
        options.Arguments.reserve(options.Arguments.size() + forwardArgs.size());
        for (const auto& arg : forwardArgs)
        {
            options.Arguments.emplace_back(WideToMultiByte(arg));
        }
    }

    if (context.Args.Contains(ArgType::WorkDir))
    {
        options.WorkingDirectory = WideToMultiByte(context.Args.Get<ArgType::WorkDir>());
    }

    context.Data.Add<Data::ContainerOptions>(std::move(options));
}

void ShowContainerStats(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();

    auto containers = context.Args.GetAll<ArgType::ContainerId>();

    // If any are specified we use those, otherwise we show all containers.
    const bool userSpecifiedContainers = !containers.empty();
    if (!userSpecifiedContainers)
    {
        GetContainers(context);
        const auto& allContainers = context.Data.Get<Data::Containers>();
        for (const auto& container : allContainers)
        {
            // Skip non-running containers unless --all is specified.
            if (!context.Args.Contains(ArgType::All) && container.State != WSLCContainerState::WslcContainerStateRunning)
            {
                continue;
            }

            containers.push_back(MultiByteToWide(container.Id));
        }
    }

    // Build stats as a json array first for later filtering or display either as json or table format.
    nlohmann::json statsJson = nlohmann::json::array();
    for (const auto& containerId : containers)
    {
        wsl::windows::common::docker_schema::ContainerStats stats;
        try
        {
            stats = ContainerService::Stats(session, WideToMultiByte(containerId));
        }
        catch (const wil::ResultException& ex)
        {
            if (!userSpecifiedContainers)
            {
                // If the user did not explicitly specify a container then there may be expected
                // race conditions between listing containers and querying stats.
                switch (ex.GetErrorCode())
                {
                case RPC_E_DISCONNECTED:
                case WSLC_E_CONTAINER_NOT_FOUND:
                    continue;
                }
            }

            LOG_HR_MSG(ex.GetErrorCode(), "Failed to get stats for container %ws", containerId.c_str());
            throw;
        }

        // Calculate CPU %
        double cpuPercent = 0.0;
        const auto cpuDelta = static_cast<double>(stats.cpu_stats.cpu_usage.total_usage) -
                              static_cast<double>(stats.precpu_stats.cpu_usage.total_usage);
        const auto systemDelta =
            static_cast<double>(stats.cpu_stats.system_cpu_usage) - static_cast<double>(stats.precpu_stats.system_cpu_usage);
        const auto onlineCpus = stats.cpu_stats.online_cpus > 0 ? stats.cpu_stats.online_cpus : 1u;
        if (systemDelta > 0.0 && cpuDelta >= 0.0)
        {
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
        const auto cpuPercentStr = std::format("{:.2f}%", cpuPercent);
        const auto memPercentStr = std::format("{:.2f}%", memPercent);
        const auto memUsage = std::format("{} / {}", FormatBytes(stats.memory_stats.usage), FormatBytes(stats.memory_stats.limit));
        const auto netIo = std::format("{} / {}", FormatBytes(netRxBytes), FormatBytes(netTxBytes));
        const auto blkIo = std::format("{} / {}", FormatBytes(blkReadBytes), FormatBytes(blkWriteBytes));

        statsJson.push_back({
            {"ID", stats.id},
            {"Name", containerName},
            {"CPUPerc", cpuPercentStr},
            {"MemUsage", memUsage},
            {"MemPerc", memPercentStr},
            {"NetIO", netIo},
            {"BlockIO", blkIo},
            {"PIDs", stats.pids_stats.current},
        });
    }

    FormatType format = FormatType::Table; // Default is table
    if (context.Args.Contains(ArgType::Format))
    {
        format = validation::GetFormatTypeFromString(context.Args.Get<ArgType::Format>());
    }

    switch (format)
    {
    case FormatType::Json:
    {
        PrintMessage(MultiByteToWide(statsJson.dump(c_jsonPrettyPrintIndent)));
        break;
    }
    case FormatType::Table:
    {
        using Config = wsl::windows::wslc::ColumnWidthConfig;
        bool trunc = !context.Args.Contains(ArgType::NoTrunc);

        auto table = trunc ? wsl::windows::wslc::TableOutput<8>(
                                 {{{Localization::WSLCCLI_TableHeaderContainerId(), {Config::NoLimit, 12, false}},
                                   {Localization::WSLCCLI_TableHeaderName(), {Config::NoLimit, 20, true}},
                                   {Localization::WSLCCLI_TableHeaderCpuPercent(), {Config::NoLimit, Config::NoLimit, false}},
                                   {Localization::WSLCCLI_TableHeaderMemUsageLimit(), {Config::NoLimit, Config::NoLimit, false}},
                                   {Localization::WSLCCLI_TableHeaderMemPercent(), {Config::NoLimit, Config::NoLimit, false}},
                                   {Localization::WSLCCLI_TableHeaderNetIo(), {Config::NoLimit, Config::NoLimit, false}},
                                   {Localization::WSLCCLI_TableHeaderBlockIo(), {Config::NoLimit, Config::NoLimit, false}},
                                   {Localization::WSLCCLI_TableHeaderPids(), {Config::NoLimit, Config::NoLimit, false}}}})
                           : wsl::windows::wslc::TableOutput<8>(
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
            table.OutputLine({
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
    const auto& id = WideToMultiByte(context.Args.Get<ArgType::ContainerId>());
    context.ExitCode = ContainerService::Start(context.Data.Get<Data::Session>(), id, context.Args.Contains(ArgType::Attach));
}

void StopContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containersToStop = context.Args.GetAll<ArgType::ContainerId>();
    StopContainerOptions options;
    if (context.Args.Contains(ArgType::Signal))
    {
        options.Signal = validation::GetWSLCSignalFromString(context.Args.Get<ArgType::Signal>());
    }

    if (context.Args.Contains(ArgType::Time))
    {
        options.Timeout = validation::GetIntegerFromString<LONG>(context.Args.Get<ArgType::Time>());
    }

    for (const auto& id : containersToStop)
    {
        ContainerService::Stop(context.Data.Get<Data::Session>(), WideToMultiByte(id), options);
    }
}

void ViewContainerLogs(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containerId = context.Args.Get<ArgType::ContainerId>();
    bool follow = context.Args.Contains(ArgType::Follow);

    ULONGLONG tail = 0;
    if (context.Args.Contains(ArgType::Tail))
    {
        tail = validation::GetIntegerFromString<ULONGLONG>(context.Args.Get<ArgType::Tail>());
    }

    ContainerService::Logs(session, WideToMultiByte(containerId), follow, tail);
}
} // namespace wsl::windows::wslc::task
