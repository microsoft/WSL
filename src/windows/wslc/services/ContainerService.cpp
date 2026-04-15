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
#include <wslutil.h>
#include <WSLCProcessLauncher.h>
#include <CommandLine.h>
#include <unordered_map>
#include <wslc.h>

namespace wsl::windows::wslc::services {
using wsl::windows::common::ClientRunningWSLCProcess;
using wsl::windows::common::wslc_schema::InspectContainer;
using wsl::windows::common::wslutil::PrintMessage;
using namespace wsl::windows::common::wslutil;
using namespace wsl::shared;
using namespace wsl::windows::wslc::models;
using namespace std::chrono_literals;

static void SetContainerArguments(WSLCProcessOptions& options, std::vector<const char*>& argsStorage)
{
    options.CommandLine = {.Values = argsStorage.data(), .Count = static_cast<ULONG>(argsStorage.size())};
}

static wsl::windows::common::RunningWSLCContainer CreateInternal(Session& session, const std::string& image, const ContainerOptions& options)
{
    auto processFlags = WSLCProcessFlagsNone;
    WI_SetFlagIf(processFlags, WSLCProcessFlagsStdin, options.Interactive);
    WI_SetFlagIf(processFlags, WSLCProcessFlagsTty, options.TTY);

    auto containerFlags = WSLCContainerFlagsNone;
    WI_SetFlagIf(containerFlags, WSLCContainerFlagsRm, options.Remove);

    wsl::windows::common::WSLCContainerLauncher containerLauncher(
        image, options.Name, options.Arguments, options.EnvironmentVariables, WSLCContainerNetworkTypeBridged, processFlags);

    // Set port options if provided
    for (const auto& port : options.Ports)
    {
        auto portMapping = PublishPort::Parse(port);

        {
            // https://github.com/microsoft/WSL/issues/14433
            // The following scenarios are currently not implemented:
            // - Ephemeral host port mappings
            // - Host port mappings with a specific host IP
            // - Host port mappings with UDP protocol
            if (portMapping.HostPort().IsEphemeral() || portMapping.HostIP().has_value() ||
                portMapping.PortProtocol() == PublishPort::Protocol::UDP)
            {
                THROW_HR_WITH_USER_ERROR(
                    HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
                    "Port mappings with ephemeral host ports, specific host IPs, or UDP protocol are not currently supported");
            }
        }

        auto containerPort = portMapping.ContainerPort();
        for (uint16_t i = 0; i < containerPort.Count(); ++i)
        {
            auto currentContainerPort = static_cast<uint16_t>(containerPort.Start() + i);
            auto currentHostPort = static_cast<uint16_t>(portMapping.HostPort().Start() + i);
            containerLauncher.AddPort(currentHostPort, currentContainerPort, AF_INET);
        }
    }

    // Add volumes if specified
    for (const auto& volumeSpec : options.Volumes)
    {
        auto volume = VolumeMount::Parse(volumeSpec);
        auto host = volume.HostPath();
        auto container = volume.ContainerPath();
        containerLauncher.AddVolume(host, container, volume.IsReadOnly());
    }

    containerLauncher.SetContainerFlags(containerFlags);

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

    for (const auto& tmpfsSpec : options.Tmpfs)
    {
        auto tmpfsMount = TmpfsMount::Parse(tmpfsSpec);
        containerLauncher.AddTmpfs(tmpfsMount.ContainerPath(), tmpfsMount.Options());
    }

    auto [result, runningContainer] = containerLauncher.CreateNoThrow(*session.Get());
    if (result == WSLC_E_IMAGE_NOT_FOUND)
    {
        {
            // Attempt to pull the image if not found
            ImageProgressCallback callback;
            PrintMessage(Localization::WSLCCLI_ImageNotFoundPulling(wsl::shared::string::MultiByteToWide(image)), stderr);
            ImageService imageService;
            imageService.Pull(session, image, &callback);
        }
        return containerLauncher.Create(*session.Get());
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

int ContainerService::Attach(Session& session, const std::string& id)
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
        if (!ConsoleService::RelayInteractiveTty(runningProcess, stdinLogs.Release().get(), true))
        {
            wsl::windows::common::wslutil::PrintMessage(L"[detached]", stderr);
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

int ContainerService::Run(Session& session, const std::string& image, ContainerOptions runOptions)
{
    // Create the container
    auto runningContainer = CreateInternal(session, image, runOptions);
    auto& container = runningContainer.Get();

    // Start the created container
    WSLCContainerStartFlags startFlags{};
    WI_SetFlagIf(startFlags, WSLCContainerStartFlagsAttach, !runOptions.Detach);
    THROW_IF_FAILED(container.Start(startFlags, nullptr)); // TODO: Error message, detach keys

    // Disable auto-delete only after successful start
    runningContainer.SetDeleteOnClose(false);

    // Handle attach if requested
    if (WI_IsFlagSet(startFlags, WSLCContainerStartFlagsAttach))
    {
        ConsoleService consoleService;
        return consoleService.AttachToCurrentConsole(runningContainer.GetInitProcess());
    }

    WSLCContainerId containerId{};
    THROW_IF_FAILED(container.GetId(containerId));
    PrintMessage(L"%hs", stdout, containerId);
    return 0;
}

CreateContainerResult ContainerService::Create(Session& session, const std::string& image, ContainerOptions runOptions)
{
    auto runningContainer = CreateInternal(session, image, runOptions);
    runningContainer.SetDeleteOnClose(false);
    auto& container = runningContainer.Get();
    WSLCContainerId id{};
    THROW_IF_FAILED(container.GetId(id));
    return {.Id = id};
}

int ContainerService::Start(Session& session, const std::string& id, bool attach)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    WSLCContainerStartFlags flags = attach ? WSLCContainerStartFlagsAttach : WSLCContainerStartFlagsNone;
    THROW_IF_FAILED_EXCEPT(container->Start(flags, nullptr), WSLC_E_CONTAINER_IS_RUNNING);

    if (!attach)
    {
        return 0;
    }

    wil::com_ptr<IWSLCProcess> process;
    THROW_IF_FAILED(container->GetInitProcess(&process));

    WSLCProcessFlags processFlags{};
    THROW_IF_FAILED(process->GetFlags(&processFlags));
    ClientRunningWSLCProcess runningProcess(std::move(process), processFlags);

    ConsoleService consoleService;
    return consoleService.AttachToCurrentConsole(std::move(runningProcess));
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

std::vector<ContainerInformation> ContainerService::List(Session& session)
{
    std::vector<ContainerInformation> result;
    wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
    wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
    THROW_IF_FAILED(session.Get()->ListContainers(&containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

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

int ContainerService::Exec(Session& session, const std::string& id, ContainerOptions options)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));

    auto execFlags = WSLCProcessFlagsNone;
    WI_SetFlagIf(execFlags, WSLCProcessFlagsStdin, options.Interactive);
    WI_SetFlagIf(execFlags, WSLCProcessFlagsTty, options.TTY);

    auto processLauncher = wsl::windows::common::WSLCProcessLauncher({}, options.Arguments, options.EnvironmentVariables, execFlags);
    if (options.User.has_value())
    {
        auto user = options.User.value();
        processLauncher.SetUser(std::move(user));
    }
    if (!options.WorkingDirectory.empty())
    {
        processLauncher.SetWorkingDirectory(std::move(options.WorkingDirectory));
    }

    return ConsoleService::AttachToCurrentConsole(processLauncher.Launch(*container));
}

InspectContainer ContainerService::Inspect(Session& session, const std::string& id)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));
    wil::unique_cotaskmem_ansistring output;
    THROW_IF_FAILED(container->Inspect(&output));
    return wsl::shared::FromJson<InspectContainer>(output.get());
}

void ContainerService::Logs(Session& session, const std::string& id, bool follow)
{
    wil::com_ptr<IWSLCContainer> container;
    THROW_IF_FAILED(session.Get()->OpenContainer(id.c_str(), &container));

    COMOutputHandle stdoutHandle;
    COMOutputHandle stderrHandle;
    WSLCLogsFlags flags = WSLCLogsFlagsNone;
    WI_SetFlagIf(flags, WSLCLogsFlagsFollow, follow);

    THROW_IF_FAILED(container->Logs(flags, &stdoutHandle, &stderrHandle, 0, 0, 0));

    wsl::windows::common::relay::MultiHandleWait io;
    io.AddHandle(std::make_unique<wsl::windows::common::relay::RelayHandle<wsl::windows::common::relay::ReadHandle>>(
        stdoutHandle.Release(), GetStdHandle(STD_OUTPUT_HANDLE)));

    if (!stderrHandle.Empty()) // This handle is only used for non-tty processes.
    {
        io.AddHandle(std::make_unique<wsl::windows::common::relay::RelayHandle<wsl::windows::common::relay::ReadHandle>>(
            stderrHandle.Release(), GetStdHandle(STD_ERROR_HANDLE)));
    }

    // TODO: Handle ctrl-c.
    io.Run({});
}
} // namespace wsl::windows::wslc::services
