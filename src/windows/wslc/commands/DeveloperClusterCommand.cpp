/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DeveloperClusterCommand.cpp

Abstract:

    Standalone developer cluster orchestration through the AKS Developer CLI.

--*/
#include "precomp.h"
#include "ArgumentConvertedTypes.h"
#include "DeveloperClusterCommand.h"
#include "Exceptions.h"
#include "SubProcess.h"

#include <filesystem>

using namespace wsl::windows::common;
using namespace wsl::windows::wslc::execution;
using namespace wsl::shared;

namespace wsl::windows::wslc {
namespace {

    constexpr auto c_aksDevCtlEnvironment = L"AKS_DEVCTL_BIN";
    constexpr auto c_aksDesktopEnvironment = L"AKS_DESKTOP_BIN";
    constexpr auto c_legacyAksEdgeEnvironment = L"AKSEDGE_BIN";

    std::wstring GetEnvironmentValue(const wchar_t* name)
    {
        while (true)
        {
            const auto length = GetEnvironmentVariableW(name, nullptr, 0);
            if (length == 0)
            {
                return {};
            }

            std::wstring value(length, L'\0');
            const auto copied = GetEnvironmentVariableW(name, value.data(), gsl::narrow<DWORD>(value.size()));
            THROW_LAST_ERROR_IF(copied == 0);
            if (copied < value.size())
            {
                value.resize(copied);
                return value;
            }
        }
    }

    std::filesystem::path GetExecutableDirectory()
    {
        std::wstring path(MAX_PATH, L'\0');
        while (true)
        {
            const auto copied = GetModuleFileNameW(nullptr, path.data(), gsl::narrow<DWORD>(path.size()));
            THROW_LAST_ERROR_IF(copied == 0);
            if (copied < path.size() - 1)
            {
                path.resize(copied);
                return std::filesystem::path{path}.parent_path();
            }

            path.resize(path.size() * 2);
        }
    }

    std::wstring FindOnPath(const wchar_t* fileName)
    {
        const auto required = SearchPathW(nullptr, fileName, nullptr, 0, nullptr, nullptr);
        if (required == 0)
        {
            return {};
        }

        std::wstring path(required, L'\0');
        const auto copied = SearchPathW(nullptr, fileName, nullptr, gsl::narrow<DWORD>(path.size()), path.data(), nullptr);
        THROW_LAST_ERROR_IF(copied == 0 || copied >= path.size());
        path.resize(copied);
        return path;
    }

    std::wstring ResolveAksDevCtl(ArgMap& args)
    {
        std::vector<std::filesystem::path> candidates;
        if (args.Contains(ArgType::DeveloperAksDevCtlPath) || args.Contains(ArgType::DeveloperAksDesktopPath) ||
            args.Contains(ArgType::DeveloperAksEdgePath))
        {
            const std::filesystem::path configured{
                args.Contains(ArgType::DeveloperAksDevCtlPath)    ? args.GetValue<ArgType::DeveloperAksDevCtlPath>()
                : args.Contains(ArgType::DeveloperAksDesktopPath) ? args.GetValue<ArgType::DeveloperAksDesktopPath>()
                                                                  : args.GetValue<ArgType::DeveloperAksEdgePath>()};
            std::error_code error;
            if (std::filesystem::is_regular_file(configured, error))
            {
                return configured.wstring();
            }

            throw ExecutionException(Localization::WSLCCLI_DeveloperClusterDevCtlNotFound());
        }

        if (const auto configured = GetEnvironmentValue(c_aksDevCtlEnvironment); !configured.empty())
        {
            candidates.emplace_back(configured);
        }

        if (const auto configured = GetEnvironmentValue(c_aksDesktopEnvironment); !configured.empty())
        {
            candidates.emplace_back(configured);
        }

        if (const auto configured = GetEnvironmentValue(c_legacyAksEdgeEnvironment); !configured.empty())
        {
            candidates.emplace_back(configured);
        }

        candidates.emplace_back(GetExecutableDirectory() / L"aks-devctl.exe");
        for (const auto& candidate : candidates)
        {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error))
            {
                return candidate.wstring();
            }
        }

        if (const auto onPath = FindOnPath(L"aks-devctl.exe"); !onPath.empty())
        {
            return onPath;
        }

        throw ExecutionException(Localization::WSLCCLI_DeveloperClusterDevCtlNotFound());
    }

    void AddValue(std::vector<std::wstring>& command, ArgMap& args, ArgType type, std::wstring_view option)
    {
#define ADD_DEVELOPER_VALUE(EnumName) \
    case ArgType::EnumName: \
        if (args.Contains(ArgType::EnumName)) \
        { \
            command.emplace_back(option); \
            command.emplace_back(args.GetValue<ArgType::EnumName>()); \
        } \
        return

        switch (type)
        {
            ADD_DEVELOPER_VALUE(DeveloperName);
            ADD_DEVELOPER_VALUE(DeveloperKubernetesVersion);
            ADD_DEVELOPER_VALUE(DeveloperPodCidr);
            ADD_DEVELOPER_VALUE(ClusterDistribution);
            ADD_DEVELOPER_VALUE(DeveloperGpuVendor);
            ADD_DEVELOPER_VALUE(DeveloperNvidiaGpuSharing);
            ADD_DEVELOPER_VALUE(ClusterOutput);
            ADD_DEVELOPER_VALUE(DeveloperDiagnosticsOutput);
            ADD_DEVELOPER_VALUE(DeveloperDiagnosticsSince);
        default:
            THROW_HR(E_UNEXPECTED);
        }
#undef ADD_DEVELOPER_VALUE
    }

    void AddFlag(std::vector<std::wstring>& command, ArgMap& args, ArgType type, std::wstring_view option)
    {
        bool present = false;
        switch (type)
        {
        case ArgType::ClusterEnableGpu:
            present = args.GetValue<ArgType::ClusterEnableGpu>();
            break;
        case ArgType::DeveloperOverwrite:
            present = args.GetValue<ArgType::DeveloperOverwrite>();
            break;
        case ArgType::DeveloperOverwriteExisting:
            present = args.GetValue<ArgType::DeveloperOverwriteExisting>();
            break;
        case ArgType::DeveloperYes:
            present = args.GetValue<ArgType::DeveloperYes>();
            break;
        case ArgType::DeveloperRedact:
            present = args.GetValue<ArgType::DeveloperRedact>();
            break;
        default:
            THROW_HR(E_UNEXPECTED);
        }

        if (present)
        {
            command.emplace_back(option);
        }
    }

    std::wstring BuildAksDevCtlCommandLine(const std::vector<std::wstring>& arguments)
    {
        std::vector<std::wstring_view> views;
        views.reserve(arguments.size());
        for (const auto& argument : arguments)
        {
            views.emplace_back(argument);
        }

        return wil::ArgvToCommandLine(views);
    }

    DWORD RunAksDevCtl(CLIExecutionContext& context, std::vector<std::wstring> arguments)
    {
        const auto executable = ResolveAksDevCtl(context.Args);
        arguments.insert(arguments.begin(), executable);
        const auto commandLine = BuildAksDevCtlCommandLine(arguments);

        wil::unique_handle job{CreateJobObjectW(nullptr, nullptr)};
        THROW_LAST_ERROR_IF(!job);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));

        SubProcess process(executable.c_str(), commandLine.c_str());
        process.SetJobObject(job.get());
        const auto cancelEvent = context.CreateCancelEvent();
        const auto processHandle = process.Start();
        const HANDLE waitHandles[] = {processHandle.get(), cancelEvent};
        const auto wait = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0 + 1)
        {
            context.Terminal.Warn(L"\n{}\n", Localization::WSLCCLI_DeveloperClusterDevCtlCancelCleanup());
            if (WaitForSingleObject(processHandle.get(), 10'000) == WAIT_TIMEOUT)
            {
                THROW_IF_WIN32_BOOL_FALSE(TerminateJobObject(job.get(), ERROR_CANCELLED));
            }
            THROW_HR(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        }

        THROW_LAST_ERROR_IF(wait != WAIT_OBJECT_0);
        return SubProcess::GetExitCode(processHandle.get());
    }

} // namespace

std::vector<Argument> GetDeveloperClusterArguments(DeveloperClusterOperation operation)
{
    std::vector<Argument> arguments{
        Argument::Create(ArgType::ClusterDeveloper),
        Argument::Create(ArgType::DeveloperAksDevCtlPath),
        Argument::Create(ArgType::DeveloperAksDesktopPath),
        Argument::Create(ArgType::DeveloperAksEdgePath)};
    switch (operation)
    {
    case DeveloperClusterOperation::Create:
        arguments.insert(
            arguments.end(),
            {Argument::Create(ArgType::DeveloperName),
             Argument::Create(ArgType::DeveloperKubernetesVersion),
             Argument::Create(ArgType::DeveloperPodCidr),
             Argument::Create(ArgType::ClusterDistribution),
             Argument::Create(ArgType::ClusterEnableGpu),
             Argument::Create(ArgType::DeveloperGpuVendor),
             Argument::Create(ArgType::DeveloperNvidiaGpuSharing),
             Argument::Create(ArgType::ClusterOutput),
             Argument::Create(ArgType::DeveloperOverwrite),
             Argument::Create(ArgType::DeveloperOverwriteExisting)});
        break;
    case DeveloperClusterOperation::Delete:
        arguments.insert(arguments.end(), {Argument::Create(ArgType::DeveloperName), Argument::Create(ArgType::DeveloperYes)});
        break;
    case DeveloperClusterOperation::Status:
        arguments.push_back(Argument::Create(ArgType::DeveloperName));
        break;
    case DeveloperClusterOperation::Kubeconfig:
        arguments.insert(
            arguments.end(),
            {Argument::Create(ArgType::DeveloperName), Argument::Create(ArgType::ClusterOutput), Argument::Create(ArgType::DeveloperOverwriteExisting)});
        break;
    case DeveloperClusterOperation::Diagnostics:
        arguments.insert(
            arguments.end(),
            {Argument::Create(ArgType::DeveloperName),
             Argument::Create(ArgType::DeveloperDiagnosticsOutput, {.Required = true}),
             Argument::Create(ArgType::DeveloperDiagnosticsSince),
             Argument::Create(ArgType::DeveloperRedact)});
        break;
    case DeveloperClusterOperation::Versions:
    case DeveloperClusterOperation::Distributions:
    case DeveloperClusterOperation::Cnis:
        break;
    }

    return arguments;
}

void ValidateDeveloperClusterArguments(DeveloperClusterOperation operation, ArgMap& args)
{
    const auto pathCount = args.Contains(ArgType::DeveloperAksDevCtlPath) + args.Contains(ArgType::DeveloperAksDesktopPath) +
                           args.Contains(ArgType::DeveloperAksEdgePath);
    if (pathCount > 1)
    {
        throw ArgumentException(
            Localization::WSLCCLI_DeveloperClusterDevCtlExecutableConflict(),
            {Argument::Create(ArgType::DeveloperAksDevCtlPath),
             Argument::Create(ArgType::DeveloperAksDesktopPath),
             Argument::Create(ArgType::DeveloperAksEdgePath)});
    }

    if (!args.GetValue<ArgType::ClusterDeveloper>())
    {
        throw ArgumentException(Localization::WSLCCLI_DeveloperClusterRequiresDeveloper(), Argument::Create(ArgType::ClusterDeveloper));
    }

    if ((operation == DeveloperClusterOperation::Create || operation == DeveloperClusterOperation::Delete ||
         operation == DeveloperClusterOperation::Kubeconfig || operation == DeveloperClusterOperation::Diagnostics) &&
        !args.Contains(ArgType::DeveloperName))
    {
        throw ArgumentException(
            Localization::WSLCCLI_DeveloperClusterNameRequired(),
            {Argument::Create(ArgType::ClusterDeveloper), Argument::Create(ArgType::DeveloperName)});
    }

    if (args.GetValue<ArgType::DeveloperOverwrite>() && !args.Contains(ArgType::ClusterOutput))
    {
        throw ArgumentException(
            Localization::WSLCCLI_DeveloperClusterOverwriteRequiresOutput(),
            {Argument::Create(ArgType::DeveloperOverwrite), Argument::Create(ArgType::ClusterOutput)});
    }
}

std::vector<std::wstring> BuildDeveloperClusterCommand(DeveloperClusterOperation operation, ArgMap& args)
{
    std::vector<std::wstring> command;
    switch (operation)
    {
    case DeveloperClusterOperation::Create:
    {
        command = {L"cluster", L"create", L"--output", L"json"};
        AddValue(command, args, ArgType::DeveloperName, L"--name");
        AddValue(command, args, ArgType::DeveloperKubernetesVersion, L"--kubernetes-version");
        AddValue(command, args, ArgType::DeveloperPodCidr, L"--pod-cidr");
        AddValue(command, args, ArgType::ClusterDistribution, L"--kubernetes-distribution");
        AddValue(command, args, ArgType::DeveloperGpuVendor, L"--gpu-vendor");
        AddValue(command, args, ArgType::DeveloperNvidiaGpuSharing, L"--nvidia-gpu-sharing");
        AddValue(command, args, ArgType::ClusterOutput, L"--kubeconfig-file");
        AddFlag(command, args, ArgType::ClusterEnableGpu, L"--enable-gpu");
        AddFlag(command, args, ArgType::DeveloperOverwrite, L"--overwrite");
        AddFlag(command, args, ArgType::DeveloperOverwriteExisting, L"--overwrite-existing");
        break;
    }
    case DeveloperClusterOperation::Delete:
        command = {L"cluster", L"delete", L"--output", L"json"};
        AddValue(command, args, ArgType::DeveloperName, L"--name");
        AddFlag(command, args, ArgType::DeveloperYes, L"--yes");
        break;
    case DeveloperClusterOperation::Status:
        command = {L"cluster", L"show"};
        AddValue(command, args, ArgType::DeveloperName, L"--name");
        break;
    case DeveloperClusterOperation::Kubeconfig:
        command = {L"cluster", L"get-credentials"};
        AddValue(command, args, ArgType::DeveloperName, L"--name");
        AddValue(command, args, ArgType::ClusterOutput, L"--file");
        AddFlag(command, args, ArgType::DeveloperOverwriteExisting, L"--overwrite-existing");
        break;
    case DeveloperClusterOperation::Diagnostics:
        command = {L"cluster", L"get-logs"};
        AddValue(command, args, ArgType::DeveloperName, L"--name");
        AddValue(command, args, ArgType::DeveloperDiagnosticsOutput, L"--output-file");
        AddValue(command, args, ArgType::DeveloperDiagnosticsSince, L"--since");
        AddFlag(command, args, ArgType::DeveloperRedact, L"--redact-network-details");
        break;
    case DeveloperClusterOperation::Versions:
        command = {L"cluster", L"get-versions"};
        break;
    case DeveloperClusterOperation::Distributions:
    case DeveloperClusterOperation::Cnis:
        command = {L"cluster", L"get-distributions"};
        break;
    }

    return command;
}

void ExecuteDeveloperCluster(DeveloperClusterOperation operation, CLIExecutionContext& context)
{
    const auto exitCode = RunAksDevCtl(context, BuildDeveloperClusterCommand(operation, context.Args));
    if (exitCode != 0)
    {
        context.ExitCode = static_cast<int>(exitCode);
    }
}

std::wstring DeveloperClusterShortDescription(DeveloperClusterOperation operation)
{
    switch (operation)
    {
    case DeveloperClusterOperation::Create:
        return Localization::WSLCCLI_DeveloperClusterCreateDesc();
    case DeveloperClusterOperation::Delete:
        return Localization::WSLCCLI_DeveloperClusterDeleteDesc();
    case DeveloperClusterOperation::Status:
        return Localization::WSLCCLI_DeveloperClusterStatusDesc();
    case DeveloperClusterOperation::Kubeconfig:
        return Localization::WSLCCLI_DeveloperClusterKubeconfigDesc();
    case DeveloperClusterOperation::Diagnostics:
        return Localization::WSLCCLI_DeveloperClusterDiagnosticsDesc();
    case DeveloperClusterOperation::Versions:
        return Localization::WSLCCLI_DeveloperClusterVersionsDesc();
    case DeveloperClusterOperation::Distributions:
        return Localization::WSLCCLI_DeveloperClusterDistributionsDesc();
    case DeveloperClusterOperation::Cnis:
        return Localization::WSLCCLI_DeveloperClusterCnisDesc();
    }

    THROW_HR(E_UNEXPECTED);
}

std::wstring DeveloperClusterLongDescription(DeveloperClusterOperation operation)
{
    switch (operation)
    {
    case DeveloperClusterOperation::Create:
        return Localization::WSLCCLI_DeveloperClusterCreateLongDesc();
    case DeveloperClusterOperation::Delete:
        return Localization::WSLCCLI_DeveloperClusterDeleteLongDesc();
    case DeveloperClusterOperation::Status:
        return Localization::WSLCCLI_DeveloperClusterStatusLongDesc();
    case DeveloperClusterOperation::Kubeconfig:
        return Localization::WSLCCLI_DeveloperClusterDevCtlKubeconfigLongDesc();
    case DeveloperClusterOperation::Diagnostics:
        return Localization::WSLCCLI_DeveloperClusterDiagnosticsLongDesc();
    case DeveloperClusterOperation::Versions:
        return Localization::WSLCCLI_DeveloperClusterDevCtlVersionsLongDesc();
    case DeveloperClusterOperation::Distributions:
        return Localization::WSLCCLI_DeveloperClusterDistributionsLongDesc();
    case DeveloperClusterOperation::Cnis:
        return Localization::WSLCCLI_DeveloperClusterDevCtlCnisLongDesc();
    }

    THROW_HR(E_UNEXPECTED);
}

std::vector<Argument> DeveloperClusterActionCommand::GetArguments() const
{
    return GetDeveloperClusterArguments(m_operation);
}

void DeveloperClusterActionCommand::ValidateArgumentsInternal(ArgMap& args) const
{
    ValidateDeveloperClusterArguments(m_operation, args);
}

void DeveloperClusterActionCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    ExecuteDeveloperCluster(m_operation, context);
}

std::wstring DeveloperClusterActionCommand::ShortDescription() const
{
    return DeveloperClusterShortDescription(m_operation);
}

std::wstring DeveloperClusterActionCommand::LongDescription() const
{
    return DeveloperClusterLongDescription(m_operation);
}

} // namespace wsl::windows::wslc
