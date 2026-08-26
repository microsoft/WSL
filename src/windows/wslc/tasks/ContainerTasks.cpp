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

} // namespace

namespace wsl::windows::wslc::task {

constexpr uint32_t c_reclaimedSpacePrecision = 4;

static bool TryInspectContainer(Terminal& terminal, Session& session, const std::string& containerId, std::optional<wslc_schema::InspectContainer>& inspectData)
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

    context.Data.Add<Data::Containers>(ContainerService::List(session, context.Args.GetValue<ArgType::All>(), limit, filters));
}

void InspectContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containerIds = context.Args.GetAllValues<ArgType::ContainerId>();
    std::vector<wsl::windows::common::wslc_schema::InspectContainer> result;
    for (const auto& id : containerIds)
    {
        std::optional<wslc_schema::InspectContainer> inspectData;
        if (TryInspectContainer(context.Terminal, session, WideToMultiByte(id), inspectData))
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

void KillContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containerIds = context.Args.GetAllValues<ArgType::ContainerId>();
    const auto signal = context.Args.GetValue<ArgType::Signal>(WSLCSignalSIGKILL);

    for (const auto& id : containerIds)
    {
        ContainerService::Kill(session, WideToMultiByte(id), signal);
        context.Terminal.Output(L"{}\n", id);
    }
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

    // Determine copy direction by looking for CONTAINER:PATH patterns.
    // A single letter before ':' is a Windows drive path (e.g. C:\path), not a container reference.
    auto isContainerPath = [](const std::wstring& path) -> bool {
        auto colonPos = path.find(L':');
        if (colonPos == std::wstring::npos || colonPos == 0)
        {
            return false;
        }

        // Single letter before colon is a Windows drive path
        if (colonPos == 1 && std::isalpha(static_cast<unsigned char>(path[0])))
        {
            return false;
        }

        return true;
    };

    auto parseContainerPath = [](const std::wstring& path) -> std::pair<std::string, std::string> {
        auto colonPos = path.find(L':');
        // Skip Windows drive letter if present
        if (colonPos == 1 && std::isalpha(static_cast<unsigned char>(path[0])))
        {
            colonPos = path.find(L':', 2);
        }

        auto container = WideToMultiByte(path.substr(0, colonPos));
        auto containerPath = WideToMultiByte(path.substr(colonPos + 1));
        return {container, containerPath};
    };

    bool sourceIsStdin = (source == L"-");
    bool sourceIsContainer = !sourceIsStdin && isContainerPath(source);
    bool targetIsContainer = isContainerPath(target);

    if ((sourceIsStdin || !sourceIsContainer) && targetIsContainer)
    {
        // stdin/local → container
        auto [containerId, destPath] = parseContainerPath(target);
        THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::WSLCCLI_CpInvalidTargetError(), containerId.empty() || destPath.empty());

        if (sourceIsStdin)
        {
            auto inputHandle = GetStdHandle(STD_INPUT_HANDLE);
            THROW_HR_WITH_USER_ERROR_IF(
                E_INVALIDARG, Localization::WSLCCLI_CpStdinIsTerminalError(), wsl::windows::common::wslutil::IsConsoleHandle(inputHandle));

            LARGE_INTEGER fileSize{};
            ULONGLONG contentSize = 0;
            if (GetFileSizeEx(inputHandle, &fileSize))
            {
                contentSize = static_cast<ULONGLONG>(fileSize.QuadPart);
            }

            // Note: The --archive/-a flag is accepted for CLI compatibility with docker cp, but is a
            // no-op here. Since the tar archive contains uid/gid ownership in its headers, and Docker's
            // PUT /archive extracts preserving that metadata.
            ContainerService::CopyToContainer(session, containerId, destPath, inputHandle, contentSize);
        }
        else
        {
            // Local path → container: create tar from local path using tar.exe
            std::error_code fsError;
            bool pathExists = std::filesystem::exists(source, fsError);
            THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::WSLCCLI_CpSourceNotFoundError(source), fsError || !pathExists);

            auto absPath = std::filesystem::absolute(source);
            auto parentDir = absPath.parent_path().wstring();
            auto fileName = absPath.filename().wstring();

            // Strip trailing separator to avoid the CRT parsing '\"' as an escaped quote
            while (parentDir.size() > 1 && (parentDir.back() == L'\\' || parentDir.back() == L'/'))
            {
                parentDir.pop_back();
            }

            // Create a temp file with DELETE_ON_CLOSE and InheritHandle so tar can write to it via stdout
            filesystem::TempFile tarFile(
                GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, filesystem::TempFileFlags::DeleteOnClose | filesystem::TempFileFlags::InheritHandle);

            // Run tar.exe writing to stdout, redirected to our temp file handle
            auto tarCmd = std::format(L"tar.exe -cf - -C \"{}\" \"{}\"", parentDir, fileName);
            SubProcess process(nullptr, tarCmd.c_str());
            process.SetStdHandles(nullptr, tarFile.Handle.get(), nullptr);
            auto exitCode = process.Run();
            THROW_HR_IF_MSG(E_FAIL, exitCode != 0, "tar.exe exited with code %u", exitCode);

            // Rewind and get size for upload
            LARGE_INTEGER zero{};
            THROW_LAST_ERROR_IF(!SetFilePointerEx(tarFile.Handle.get(), zero, nullptr, FILE_BEGIN));

            LARGE_INTEGER fileSize{};
            THROW_LAST_ERROR_IF(!GetFileSizeEx(tarFile.Handle.get(), &fileSize));

            ContainerService::CopyToContainer(
                session, containerId, destPath, tarFile.Handle.get(), static_cast<ULONGLONG>(fileSize.QuadPart));
        }
    }
    else if (sourceIsContainer && !targetIsContainer)
    {
        // container → local
        auto [containerId, srcPath] = parseContainerPath(source);
        THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::WSLCCLI_CpInvalidSourceError(), containerId.empty() || srcPath.empty());

        // Resolve any symlinks in the target path since tar.exe refuses to extract through a symlink.
        std::error_code canonicalError;
        auto absTarget = wsl::windows::common::filesystem::GetCanonicalPath(target, canonicalError);
        if (canonicalError)
        {
            absTarget = std::filesystem::absolute(target); // Fall back to absolute if canonicalization fails.
        }

        // Determine if target is a directory or a file destination.
        // Treat as directory if: ends with separator, or already exists as a directory.
        bool targetIsDir = (!target.empty() && (target.back() == L'\\' || target.back() == L'/')) || std::filesystem::is_directory(absTarget);

        if (targetIsDir)
        {
            // Extract directly into the target directory by piping the download to tar stdin.
            std::error_code dirError;
            std::filesystem::create_directories(absTarget, dirError);
            THROW_HR_IF_MSG(HRESULT_FROM_WIN32(dirError.value()), !!dirError, "Failed to create directory: %ls", absTarget.c_str());

            // Strip trailing separator to avoid the CRT parsing a trailing '\"' as an escaped quote.
            auto targetDir = absTarget.wstring();
            while (targetDir.size() > 1 && (targetDir.back() == L'\\' || targetDir.back() == L'/'))
            {
                targetDir.pop_back();
            }

            auto [pipeRead, pipeWrite] = OpenAnonymousPipe(0, false, false);
            THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(pipeRead.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

            auto tarCmd = std::format(L"tar.exe -xf - -C \"{}\"", targetDir);
            SubProcess process(nullptr, tarCmd.c_str());
            process.SetStdHandles(pipeRead.get(), nullptr, nullptr);
            auto processHandle = process.Start();
            pipeRead.reset();

            ContainerService::CopyFromContainer(session, containerId, srcPath, pipeWrite.get());
            pipeWrite.reset();

            auto exitCode = SubProcess::GetExitCode(processHandle.get());
            THROW_HR_IF_MSG(E_FAIL, exitCode != 0, "tar.exe exited with code %u", exitCode);
        }
        else
        {
            // Target is a file path. Download archive once to a temp file (exclusive write handle),
            // validate it contains a single file with tar -t, then extract via tar -x -O.

            // Download archive to temp file. FILE_SHARE_READ allows tar to read it while we hold
            // the exclusive write handle, preventing other processes from tampering.
            filesystem::TempFile tarFile(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS);

            ContainerService::CopyFromContainer(session, containerId, srcPath, tarFile.Handle.get());

            // Step 1: Pipe tar -t output and read just enough lines to classify the archive.
            auto [listStdoutRead, listStdoutWrite] = OpenAnonymousPipe(0, true, false);
            THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(listStdoutWrite.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

            auto listCmd = std::format(L"tar.exe -tf \"{}\"", tarFile.Path.wstring());
            SubProcess listProcess(nullptr, listCmd.c_str());
            listProcess.SetStdHandles(nullptr, listStdoutWrite.get(), nullptr);
            auto listHandle = listProcess.Start();
            listStdoutWrite.reset();

            // Read lines from tar -t output. We only need to detect:
            // - zero entries (empty archive)
            // - exactly one non-directory entry (single file)
            // - anything else (directory or multi-file)
            size_t entryCount = 0;
            bool hasDirectory = false;
            std::string lineBuffer;
            char readBuf[4096];
            DWORD bytesRead = 0;
            bool done = false;
            while (!done && ReadFile(listStdoutRead.get(), readBuf, sizeof(readBuf), &bytesRead, nullptr) && bytesRead > 0)
            {
                for (DWORD i = 0; i < bytesRead && !done; i++)
                {
                    if (readBuf[i] == '\n')
                    {
                        if (!lineBuffer.empty())
                        {
                            entryCount++;
                            if (lineBuffer.back() == '/')
                            {
                                hasDirectory = true;
                            }

                            // We can stop early: directory entry or second entry means not a single file.
                            if (hasDirectory || entryCount > 1)
                            {
                                done = true;
                            }

                            lineBuffer.clear();
                        }
                    }
                    else if (readBuf[i] != '\r')
                    {
                        lineBuffer.append(1, readBuf[i]);
                    }
                }
            }

            // Count trailing line without newline.
            if (!done && !lineBuffer.empty())
            {
                entryCount++;
                if (lineBuffer.back() == '/')
                {
                    hasDirectory = true;
                }
            }

            listStdoutRead.reset();

            // Kill the tar -t process (it may still be writing lines we stopped reading) and wait for it to exit.
            TerminateProcess(listHandle.get(), 0);
            SubProcess::GetExitCode(listHandle.get());

            THROW_HR_WITH_USER_ERROR_IF(E_FAIL, Localization::WSLCCLI_CpSourceIsDirectoryError(), hasDirectory || entryCount > 1);

            THROW_HR_WITH_USER_ERROR_IF(E_FAIL, Localization::WSLCCLI_CpNoFileExtractedError(), entryCount == 0);

            // Step 2: Extract the single file content directly to the target.
            std::error_code dirError;
            std::filesystem::create_directories(absTarget.parent_path(), dirError);
            THROW_HR_IF_MSG(
                HRESULT_FROM_WIN32(dirError.value()), !!dirError, "Failed to create directory: %ls", absTarget.parent_path().c_str());

            wil::unique_hfile targetFile(CreateFileW(absTarget.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            THROW_LAST_ERROR_IF(!targetFile);
            THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(targetFile.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

            auto extractCmd = std::format(L"tar.exe -xf \"{}\" -O", tarFile.Path.wstring());
            SubProcess extractProcess(nullptr, extractCmd.c_str());
            extractProcess.SetStdHandles(nullptr, targetFile.get(), nullptr);
            auto extractHandle = extractProcess.Start();
            targetFile.reset();

            auto extractExitCode = SubProcess::GetExitCode(extractHandle.get());
            THROW_HR_IF_MSG(E_FAIL, extractExitCode != 0, "tar.exe -x -O exited with code %u", extractExitCode);
        }
    }
    else
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::WSLCCLI_CpInvalidDirectionError());
    }
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
        for (const auto& container : containers)
        {
            context.Terminal.Output(L"{}\n", MultiByteToWide(container.Id));
        }

        return;
    }

    const auto format = context.Args.GetValue<ArgType::Format>(FormatType::Table);

    switch (format)
    {
    case FormatType::Json:
    {
        for (const auto& container : containers)
        {
            context.Terminal.Output(L"{}\n", ToJsonW(container, c_jsonCompactIndent));
        }

        break;
    }
    case FormatType::Table:
    {
        bool trunc = !context.Args.GetValue<ArgType::NoTrunc>();
        using enum ColumnOverflow;

        // Create table with or without column limits based on --no-trunc flag
        auto table = trunc ? wsl::windows::wslc::TableOutput<6>(
                                 context.Terminal,
                                 {{{Localization::WSLCCLI_TableHeaderContainerId(), {.MaxWidth = 12, .Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderName(), {.MaxWidth = 20, .Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderImage(), {.MaxWidth = 20, .Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderCreated(), {.Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderStatus(), {.Overflow = Shrink}},
                                   {Localization::WSLCCLI_TableHeaderPorts(), {.Overflow = Shrink}}}},
                                 containers.size())
                           : wsl::windows::wslc::TableOutput<6>(
                                 context.Terminal,
                                 {Localization::WSLCCLI_TableHeaderContainerId(),
                                  Localization::WSLCCLI_TableHeaderName(),
                                  Localization::WSLCCLI_TableHeaderImage(),
                                  Localization::WSLCCLI_TableHeaderCreated(),
                                  Localization::WSLCCLI_TableHeaderStatus(),
                                  Localization::WSLCCLI_TableHeaderPorts()});

        // Add each container as a row
        for (const auto& container : containers)
        {
            table.WriteRow({
                MultiByteToWide(trunc ? TruncateId(container.Id) : container.Id),
                MultiByteToWide(container.Name),
                MultiByteToWide(container.Image),
                FormatRelativeTime(container.CreatedAt),
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
    auto containerIds = context.Args.GetAllValues<ArgType::ContainerId>();
    bool force = context.Args.GetValue<ArgType::Force>();
    bool deleteVolumes = context.Args.GetValue<ArgType::Volumes>();
    for (const auto& id : containerIds)
    {
        ContainerService::Delete(session, WideToMultiByte(id), force, deleteVolumes);
        context.Terminal.Output(L"{}\n", id);
    }
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

        auto table = trunc ? wsl::windows::wslc::TableOutput<8>(
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
                           : wsl::windows::wslc::TableOutput<8>(
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
    auto containersToStop = context.Args.GetAllValues<ArgType::ContainerId>();
    StopContainerOptions options;

    // WSLCSignalNone lets Docker use the container's configured STOPSIGNAL, or its default when none is configured.
    options.Signal = context.Args.GetValue<ArgType::Signal>(WSLCSignalNone);

    if (context.Args.Contains(ArgType::Time))
    {
        options.Timeout = context.Args.GetValue<ArgType::Time>();
    }

    for (const auto& id : containersToStop)
    {
        ContainerService::Stop(context.Data.Get<Data::Session>(), WideToMultiByte(id), options);
        context.Terminal.Output(L"{}\n", id);
    }
}

void ViewContainerLogs(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containerId = context.Args.GetValue<ArgType::ContainerId>();
    bool follow = context.Args.GetValue<ArgType::Follow>();
    bool timestamps = context.Args.GetValue<ArgType::Timestamps>();

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

    ContainerService::Logs(session, WideToMultiByte(containerId), follow, timestamps, since, until, tail);
}

void PruneContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();

    auto result = ContainerService::Prune(session);

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
