/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerService.cpp

Abstract:

    This file contains the ContainerService implementation

--*/

#include <precomp.h>
#include "ContainerService.h"
#include "ConsoleService.h"
#include "ImageService.h"
#include "ImageProgressCallback.h"
#include "WarningCallback.h"
#include <wslutil.h>
#include <HandleConsoleProgressBar.h>
#include <WSLCProcessLauncher.h>
#include <ConsoleState.h>
#include <CommandLine.h>
#include <WSLCUserSettings.h>
#include <filesystem>
#include <unordered_map>
#include <wslc.h>

namespace wsl::windows::wslc::services {
using wsl::windows::common::ClientRunningWSLCProcess;
using wsl::windows::common::wslc_schema::InspectContainer;
using namespace wsl::windows::common::wslutil;
using namespace wsl::shared;
using namespace wsl::windows::wslc::models;
using namespace std::chrono_literals;

static void SetContainerArguments(WSLCProcessOptions& options, std::vector<const char*>& argsStorage)
{
    options.CommandLine = {.Values = argsStorage.data(), .Count = static_cast<ULONG>(argsStorage.size())};
}

static wsl::windows::common::RunningWSLCContainer CreateInternal(Reporter& reporter, Session& session, const std::string& image, const ContainerOptions& options)
{
    WarningCallback warningCallback(reporter);

    auto processFlags = WSLCProcessFlagsNone;
    WI_SetFlagIf(processFlags, WSLCProcessFlagsStdin, options.Interactive);
    WI_SetFlagIf(processFlags, WSLCProcessFlagsTty, options.TTY);

    auto containerFlags = WSLCContainerFlagsNone;
    WI_SetFlagIf(containerFlags, WSLCContainerFlagsRm, options.Remove);
    WI_SetFlagIf(containerFlags, WSLCContainerFlagsPublishAll, options.PublishAll);
    WI_SetFlagIf(containerFlags, WSLCContainerFlagsGpu, options.Gpu);

    std::string networkMode = options.Networks.empty() ? std::string("bridge") : options.Networks.front();

    wsl::windows::common::WSLCContainerLauncher containerLauncher(
        image, options.Name, options.Arguments, options.EnvironmentVariables, std::move(networkMode), processFlags);

    for (size_t i = 1; i < options.Networks.size(); ++i)
    {
        containerLauncher.AddAdditionalNetwork(options.Networks[i]);
    }

    if (!options.NetworkAliases.empty())
    {
        THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcAliasRequiresUserDefinedNetwork(), options.Networks.empty());

        THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcAliasAmbiguousWithMultipleNetworks(), options.Networks.size() > 1);

        const auto& primary = options.Networks.front();
        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG,
            Localization::MessageWslcAliasRequiresUserDefinedNetwork(),
            primary == "bridge" || primary == "host" || primary == "none" || primary.starts_with("container:"));

        for (const auto& alias : options.NetworkAliases)
        {
            containerLauncher.AddPrimaryNetworkAlias(alias);
        }
    }

    const auto defaultBindingAddress = settings::User().Get<settings::Setting::SessionDefaultBindingAddress>();

    // Set port options if provided
    for (const auto& port : options.Ports)
    {
        auto portMapping = PublishPort::Parse(port);

        const int protocol = portMapping.PortProtocol() == PublishPort::Protocol::UDP ? IPPROTO_UDP : IPPROTO_TCP;
        const int family = (portMapping.HostIP().has_value() && portMapping.HostIP()->IsIPv6()) ? AF_INET6 : AF_INET;
        std::optional<std::string> bindAddress;
        if (portMapping.HostIP().has_value())
        {
            bindAddress = portMapping.HostIP()->IP();
        }
        else if (!defaultBindingAddress.empty())
        {
            // No explicit host IP: apply the configured default binding address (IPv4 only,
            // since IPv6 bindings are always explicit). When unset, AddPort falls back to loopback.
            bindAddress = defaultBindingAddress;
        }

        auto containerPort = portMapping.ContainerPort();
        for (uint16_t i = 0; i < containerPort.Count(); ++i)
        {
            auto currentContainerPort = static_cast<uint16_t>(containerPort.Start() + i);
            auto currentHostPort = portMapping.HostPort().IsEphemeral() ? static_cast<uint16_t>(WSLC_EPHEMERAL_PORT)
                                                                        : static_cast<uint16_t>(portMapping.HostPort().Start() + i);
            containerLauncher.AddPort(currentHostPort, currentContainerPort, family, protocol, bindAddress);
        }
    }

    // Add volumes if specified
    for (const auto& volumeSpec : options.Volumes)
    {
        auto volume = VolumeMount::Parse(volumeSpec);
        auto host = volume.Host();
        auto container = volume.ContainerPath();
        if (volume.IsNamedVolume())
        {
            containerLauncher.AddNamedVolume(string::WideToMultiByte(host), container, volume.IsReadOnly());
        }
        else
        {
            containerLauncher.AddVolume(host, container, volume.IsReadOnly());
        }
    }

    containerLauncher.SetContainerFlags(containerFlags);

    if (options.StopSignal != WSLCSignalNone)
    {
        containerLauncher.SetDefaultStopSignal(options.StopSignal);
    }

    if (options.StopTimeout.has_value())
    {
        containerLauncher.SetStopTimeout(options.StopTimeout.value());
    }

    if (options.ShmSize.has_value())
    {
        containerLauncher.SetShmSize(options.ShmSize.value());
    }

    if (options.HealthCmd.has_value())
    {
        containerLauncher.SetHealthCmd(std::string(options.HealthCmd.value()));
    }

    if (options.HealthInterval.has_value())
    {
        containerLauncher.SetHealthInterval(options.HealthInterval.value());
    }

    if (options.HealthTimeout.has_value())
    {
        containerLauncher.SetHealthTimeout(options.HealthTimeout.value());
    }

    if (options.HealthStartPeriod.has_value())
    {
        containerLauncher.SetHealthStartPeriod(options.HealthStartPeriod.value());
    }

    if (options.HealthRetries.has_value())
    {
        containerLauncher.SetHealthRetries(options.HealthRetries.value());
    }

    if (options.NoHealthcheck)
    {
        containerLauncher.SetNoHealthcheck();
    }

    if (options.MemoryBytes.has_value())
    {
        containerLauncher.SetMemoryLimit(options.MemoryBytes.value());
    }

    if (options.NanoCpus.has_value())
    {
        containerLauncher.SetNanoCpus(options.NanoCpus.value());
    }

    for (const auto& [name, soft, hard] : options.Ulimits)
    {
        containerLauncher.AddUlimit(name, soft, hard);
    }

    if (!options.Entrypoint.empty())
    {
        auto entrypoints = options.Entrypoint;
        containerLauncher.SetEntrypoint(std::move(entrypoints));
    }

    if (options.User.has_value())
    {
        auto user = options.User.value();
        containerLauncher.SetUser(std::move(user));
    }

    if (!options.WorkingDirectory.empty())
    {
        containerLauncher.SetWorkingDirectory(std::string(options.WorkingDirectory));
    }

    if (options.Hostname.has_value())
    {
        containerLauncher.SetHostname(std::string(options.Hostname.value()));
    }

    if (options.Domainname.has_value())
    {
        containerLauncher.SetDomainname(std::string(options.Domainname.value()));
    }

    if (!options.DnsServers.empty())
    {
        containerLauncher.SetDnsServers(std::vector<std::string>(options.DnsServers));
    }

    if (!options.DnsSearchDomains.empty())
    {
        containerLauncher.SetDnsSearchDomains(std::vector<std::string>(options.DnsSearchDomains));
    }

    if (!options.DnsOptions.empty())
    {
        containerLauncher.SetDnsOptions(std::vector<std::string>(options.DnsOptions));
    }

    for (const auto& tmpfsSpec : options.Tmpfs)
    {
        auto tmpfsMount = TmpfsMount::Parse(tmpfsSpec);
        containerLauncher.AddTmpfs(tmpfsMount.ContainerPath(), tmpfsMount.Options());
    }

    for (const auto& [key, value] : options.Labels)
    {
        containerLauncher.AddLabel(key, value);
    }

    auto [result, runningContainer] = containerLauncher.CreateNoThrow(*session.Get(), &warningCallback);
    if (result == WSLC_E_IMAGE_NOT_FOUND)
    {
        {
            // Implicit pull for run/create: progress goes to Info (stderr), keeping stdout for the
            // container id/output.
            ImageProgressCallback callback(reporter, Reporter::Level::Info);
            reporter.Info(L"{}\n", Localization::WSLCCLI_ImageNotFoundPulling(wsl::shared::string::MultiByteToWide(image)));
            ImageService imageService;
            imageService.Pull(reporter, session, image, &callback);
        }
        return containerLauncher.Create(*session.Get(), &warningCallback);
    }

    THROW_IF_FAILED(result);
    ASSERT(runningContainer);
    return std::move(*runningContainer);
}

static PortInformation PortInformationFromWSLCPortMapping(const WSLCPortMapping& mapping)
{
    return PortInformation{
        .HostPort = mapping.HostPort,
        .ContainerPort = mapping.ContainerPort,
        .Protocol = static_cast<int>(mapping.Protocol),
        .BindingAddress = mapping.BindingAddress,
    };
}

std::wstring ContainerService::FormatRelativeTime(ULONGLONG timestamp)
{
    if (timestamp == 0)
    {
        return L"";
    }

    constexpr LONGLONG SecondsPerMinute = std::chrono::duration_cast<std::chrono::seconds>(1min).count();
    constexpr LONGLONG SecondsPerHour = std::chrono::duration_cast<std::chrono::seconds>(1h).count();
    constexpr LONGLONG SecondsPerDay = std::chrono::duration_cast<std::chrono::seconds>(24h).count();
    constexpr LONGLONG SecondsPerWeek = SecondsPerDay * 7;
    constexpr LONGLONG SecondsPerMonth = SecondsPerDay * 30;
    constexpr LONGLONG SecondsPerYear = SecondsPerDay * 365;

    auto elapsed = static_cast<LONGLONG>(std::time(nullptr)) - static_cast<LONGLONG>(timestamp);
    if (elapsed < 0)
    {
        elapsed = 0;
    }

    auto pluralize = [](LONGLONG count, const wchar_t* singular, const wchar_t* plural) {
        return std::format(L"{} {} ago", count, (count == 1 ? singular : plural));
    };

    if (elapsed < SecondsPerMinute)
    {
        return pluralize(elapsed, L"second", L"seconds");
    }
    else if (elapsed < SecondsPerHour)
    {
        return pluralize(elapsed / SecondsPerMinute, L"minute", L"minutes");
    }
    else if (elapsed < SecondsPerDay)
    {
        return pluralize(elapsed / SecondsPerHour, L"hour", L"hours");
    }
    else if (elapsed < SecondsPerWeek)
    {
        return pluralize(elapsed / SecondsPerDay, L"day", L"days");
    }
    else if (elapsed < SecondsPerMonth)
    {
        return pluralize(elapsed / SecondsPerWeek, L"week", L"weeks");
    }
    else if (elapsed < SecondsPerYear)
    {
        return pluralize(elapsed / SecondsPerMonth, L"month", L"months");
    }

    return pluralize(elapsed / SecondsPerYear, L"year", L"years");
}

int ContainerService::Attach(Reporter& reporter, Session& session, const std::string& id)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));

    wil::com_ptr<IWSLCProcess> process;
    THROW_IF_FAILED(container->GetInitProcess(&process));

    WSLCProcessFlags processFlags{};
    THROW_IF_FAILED(process->GetFlags(&processFlags));

    ClientRunningWSLCProcess runningProcess(std::move(process), processFlags);

    COMOutputHandle stdinLogs{};
    COMOutputHandle stdoutLogs{};
    COMOutputHandle stderrLogs{};
    THROW_IF_FAILED(container->Attach(nullptr, &stdinLogs, &stdoutLogs, &stderrLogs));

    if (!stdoutLogs.Empty())
    {
        // Non-TTY process - relay separate stdout/stderr streams
        WI_ASSERT(!stderrLogs.Empty());
        ConsoleService::RelayNonTtyProcess(stdinLogs.Release(), stdoutLogs.Release(), stderrLogs.Release());
    }
    else
    {
        // TTY process - relay using interactive TTY handling
        WI_ASSERT(stderrLogs.Empty());
        wsl::windows::common::ConsoleState console;
        if (!ConsoleService::RelayInteractiveTty(console, runningProcess, stdinLogs.Release().get(), true))
        {
            reporter.Info(L"[detached]\n");
            return 0; // Exit early if user detached
        }
    }

    // Wait for the container process to exit
    return runningProcess.Wait();
}

std::wstring ContainerService::ContainerStateToString(WSLCContainerState state, ULONGLONG stateChangedAt)
{
    std::wstring stateString;
    switch (state)
    {
    case WSLCContainerState::WslcContainerStateCreated:
        stateString = L"created";
        break;
    case WSLCContainerState::WslcContainerStateRunning:
        stateString = L"running";
        break;
    case WSLCContainerState::WslcContainerStateDeleted:
        stateString = L"stopped";
        break;
    case WSLCContainerState::WslcContainerStateExited:
        stateString = L"exited";
        break;
    case WSLCContainerState::WslcContainerStateInvalid:
        return L"invalid";
    default:
        THROW_HR(E_UNEXPECTED);
    }

    if (stateChangedAt == 0)
    {
        return stateString;
    }

    return std::format(L"{} {}", stateString, FormatRelativeTime(stateChangedAt));
}

std::wstring ContainerService::FormatPorts(WSLCContainerState state, const std::vector<PortInformation>& ports)
{
    if (state != WslcContainerStateRunning || ports.empty())
    {
        return L"";
    }

    std::wstring result;
    for (size_t i = 0; i < ports.size(); ++i)
    {
        const auto& port = ports[i];

        std::wstring hostIp = wsl::shared::string::MultiByteToWide(port.BindingAddress);

        std::wstring protocol = (port.Protocol == IPPROTO_TCP)   ? L"tcp"
                                : (port.Protocol == IPPROTO_UDP) ? L"udp"
                                                                 : std::format(L"{}", port.Protocol);

        if (i > 0)
        {
            result += L", ";
        }

        result += std::format(
            L"{}:{}->{}/{}", (hostIp.find(L':') != std::wstring::npos) ? std::format(L"[{}]", hostIp) : hostIp, port.HostPort, port.ContainerPort, protocol);
    }

    return result;
}

int ContainerService::Run(Reporter& reporter, Session& session, const std::string& image, ContainerOptions runOptions)
{
    // Reserve the CID file (fails if it already exists) before creating the container so a
    // container isn't created when the caller-requested path can't be written. The file is
    // removed automatically if we don't reach Commit() below.
    CidFile cidFile(runOptions.CidFile);

    // Create the container
    auto runningContainer = CreateInternal(reporter, session, image, runOptions);
    auto& container = runningContainer.Get();

    WSLCContainerId containerId{};
    THROW_IF_FAILED(container.GetId(containerId));

    // Start the created container
    WSLCContainerStartFlags startFlags{};
    WI_SetFlagIf(startFlags, WSLCContainerStartFlagsAttach, !runOptions.Detach);

    const bool attach = WI_IsFlagSet(startFlags, WSLCContainerStartFlagsAttach);

    wsl::windows::common::ConsoleState console;
    WSLCProcessStartOptions startOptions{};
    if (runOptions.TTY)
    {

        const auto size = console.GetWindowSize();
        startOptions.TtyRows = size.Y;
        startOptions.TtyColumns = size.X;
    }

    WarningCallback warningCallback(reporter);
    THROW_IF_FAILED(container.Start(startFlags, &startOptions, &warningCallback)); // TODO: detach keys

    // Disable auto-delete only after successful start
    runningContainer.SetDeleteOnClose(false);
    cidFile.Commit(containerId);

    // Handle attach if requested
    if (attach)
    {
        return ConsoleService::AttachToCurrentConsole(reporter, console, runningContainer.GetInitProcess());
    }

    reporter.Output(L"{}\n", wsl::shared::string::MultiByteToWide(containerId));
    return 0;
}

CreateContainerResult ContainerService::Create(Reporter& reporter, Session& session, const std::string& image, ContainerOptions runOptions)
{
    CidFile cidFile(runOptions.CidFile);
    auto runningContainer = CreateInternal(reporter, session, image, runOptions);
    runningContainer.SetDeleteOnClose(false);
    auto& container = runningContainer.Get();
    WSLCContainerId id{};
    THROW_IF_FAILED(container.GetId(id));
    cidFile.Commit(id);
    return {.Id = id};
}

int ContainerService::Start(Reporter& reporter, Session& session, const std::string& id, bool attach)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    WSLCContainerStartFlags flags = attach ? WSLCContainerStartFlagsAttach : WSLCContainerStartFlagsNone;

    wsl::windows::common::ConsoleState console;
    WSLCProcessStartOptions startOptions{};
    const auto size = console.GetWindowSize();
    startOptions.TtyRows = size.Y;
    startOptions.TtyColumns = size.X;

    WarningCallback warningCallback(reporter);
    THROW_IF_FAILED_EXCEPT(container->Start(flags, &startOptions, &warningCallback), WSLC_E_CONTAINER_IS_RUNNING);

    if (!attach)
    {
        return 0;
    }

    wil::com_ptr<IWSLCProcess> process;
    THROW_IF_FAILED(container->GetInitProcess(&process));

    WSLCProcessFlags processFlags{};
    THROW_IF_FAILED(process->GetFlags(&processFlags));
    ClientRunningWSLCProcess runningProcess(std::move(process), processFlags);

    return ConsoleService::AttachToCurrentConsole(reporter, console, std::move(runningProcess), true);
}

void ContainerService::Stop(Session& session, const std::string& id, StopContainerOptions options)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    THROW_IF_FAILED_EXCEPT(container->Stop(options.Signal, options.Timeout), WSLC_E_CONTAINER_NOT_RUNNING);
}

void ContainerService::Kill(Session& session, const std::string& id, WSLCSignal signal)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    THROW_IF_FAILED(container->Kill(signal));
}

void ContainerService::Delete(Session& session, const std::string& id, bool force)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    THROW_IF_FAILED(container->Delete(force ? WSLCDeleteFlagsForce : WSLCDeleteFlagsNone));
}

std::vector<ContainerInformation> ContainerService::List(
    Session& session, bool all, int limit, const std::vector<std::pair<std::string, std::string>>& filters)
{
    std::vector<WSLCFilter> filterEntries;
    filterEntries.reserve(filters.size());
    for (const auto& [key, value] : filters)
    {
        filterEntries.push_back({.Key = key.c_str(), .Value = value.c_str()});
    }

    WSLCListContainersOptions options{};
    options.Flags = all ? WSLCListContainersFlagsAll : WSLCListContainersFlagsNone;
    options.Limit = limit;
    options.Filters = filterEntries.data();
    options.FiltersCount = static_cast<ULONG>(filterEntries.size());

    wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
    wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
    THROW_IF_FAILED(
        session.Get()->ListContainers(&options, &containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

    std::vector<ContainerInformation> result;

    for (const auto& current : containers)
    {
        ContainerInformation entry;
        entry.Name = current.Name;
        entry.Image = current.Image;
        entry.State = current.State;
        entry.Id = current.Id;
        entry.StateChangedAt = current.StateChangedAt;
        entry.CreatedAt = current.CreatedAt;

        for (const auto& port : ports)
        {
            if (strcmp(port.Id, current.Id) == 0)
            {
                entry.Ports.push_back(PortInformationFromWSLCPortMapping(port.PortMapping));
            }
        }

        result.emplace_back(std::move(entry));
    }

    return result;
}

int ContainerService::Exec(Reporter& reporter, Session& session, const std::string& id, ContainerOptions options)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));

    auto execFlags = WSLCProcessFlagsNone;
    WI_SetFlagIf(execFlags, WSLCProcessFlagsStdin, options.Interactive);
    WI_SetFlagIf(execFlags, WSLCProcessFlagsTty, options.TTY);

    auto processLauncher = wsl::windows::common::WSLCProcessLauncher({}, options.Arguments, options.EnvironmentVariables, execFlags);

    wsl::windows::common::ConsoleState console;
    if (options.TTY)
    {
        const auto size = console.GetWindowSize();
        processLauncher.SetTtySize(size.Y, size.X);
    }

    if (options.User.has_value())
    {
        auto user = options.User.value();
        processLauncher.SetUser(std::move(user));
    }
    if (!options.WorkingDirectory.empty())
    {
        processLauncher.SetWorkingDirectory(std::move(options.WorkingDirectory));
    }

    return ConsoleService::AttachToCurrentConsole(reporter, console, processLauncher.Launch(*container));
}

InspectContainer ContainerService::Inspect(Session& session, const std::string& id)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    wil::unique_cotaskmem_ansistring output;
    THROW_IF_FAILED(container->Inspect(&output));
    return wsl::shared::FromJson<InspectContainer>(output.get());
}

void ContainerService::Export(Session& session, const std::string& id, const std::wstring& outputPath)
{
    wil::unique_hfile outputFile{
        CreateFileW(outputPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    THROW_LAST_ERROR_IF(!outputFile);

    Export(session, id, outputFile.get());
}

void ContainerService::Export(Session& session, const std::string& id, HANDLE outputHandle)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));

    wsl::windows::common::HandleConsoleProgressBar progressBar(
        outputHandle, Localization::MessageWslcExportInProgress(), wsl::windows::common::HandleConsoleProgressBar::Format::FileSize);

    THROW_IF_FAILED(container->Export(ToCOMInputHandle(outputHandle)));
}

void ContainerService::CopyToContainer(Session& session, const std::string& id, const std::string& destPath, HANDLE inputHandle, ULONGLONG contentSize)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));

    THROW_IF_FAILED(container->UploadArchive(ToCOMInputHandle(inputHandle), destPath.c_str(), contentSize));
}

void ContainerService::CopyFromContainer(Session& session, const std::string& id, const std::string& srcPath, HANDLE outputHandle)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));

    THROW_IF_FAILED(container->DownloadArchive(srcPath.c_str(), ToCOMInputHandle(outputHandle)));
}

void ContainerService::Logs(Session& session, const std::string& id, bool follow, bool timestamps, ULONGLONG since, ULONGLONG until, ULONGLONG tail)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));

    COMOutputHandle stdoutHandle;
    COMOutputHandle stderrHandle;
    WSLCLogsFlags flags = WSLCLogsFlagsNone;
    WI_SetFlagIf(flags, WSLCLogsFlagsFollow, follow);
    WI_SetFlagIf(flags, WSLCLogsFlagsTimestamps, timestamps);

    THROW_IF_FAILED(container->Logs(flags, &stdoutHandle, &stderrHandle, since, until, tail));

    wsl::windows::common::io::MultiHandleWait io;
    io.AddHandle(std::make_unique<wsl::windows::common::io::RelayHandle<wsl::windows::common::io::ReadHandle>>(
        stdoutHandle.Release(), GetStdHandle(STD_OUTPUT_HANDLE)));

    if (!stderrHandle.Empty()) // This handle is only used for non-tty processes.
    {
        io.AddHandle(std::make_unique<wsl::windows::common::io::RelayHandle<wsl::windows::common::io::ReadHandle>>(
            stderrHandle.Release(), GetStdHandle(STD_ERROR_HANDLE)));
    }

    // TODO: Handle ctrl-c.
    io.Run({});
}

wsl::windows::common::docker_schema::ContainerStats ContainerService::Stats(Session& session, const std::string& id)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    wil::unique_cotaskmem_ansistring output;
    THROW_IF_FAILED(container->Stats(&output));
    return wsl::shared::FromJson<wsl::windows::common::docker_schema::ContainerStats>(output.get());
}

PruneContainersResult ContainerService::Prune(Session& session)
{
    PruneResult result;
    THROW_IF_FAILED(session.Get()->PruneContainers(nullptr, 0, &result.result));

    PruneContainersResult pruneResult;
    pruneResult.SpaceReclaimed = result.result.SpaceReclaimed;
    pruneResult.PrunedContainers.reserve(result.result.ContainersCount);
    for (ULONG i = 0; i < result.result.ContainersCount; i++)
    {
        pruneResult.PrunedContainers.push_back(result.result.Containers[i]);
    }

    return pruneResult;
}
} // namespace wsl::windows::wslc::services
