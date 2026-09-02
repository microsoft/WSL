/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DeveloperClusterCommand.cpp

Abstract:

    Standalone developer cluster orchestration through the Edge Core SDK CLI.

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

    constexpr auto c_aksEdgeEnvironment = L"AKSEDGE_BIN";
    constexpr auto c_agentDebEnvironment = L"AKSEDGE_AGENT_DEB";

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

    std::wstring ResolveAksEdge(ArgMap& args)
    {
        std::vector<std::filesystem::path> candidates;
        if (args.Contains(ArgType::DeveloperAksEdgePath))
        {
            const std::filesystem::path configured{args.GetValue<ArgType::DeveloperAksEdgePath>()};
            std::error_code error;
            if (std::filesystem::is_regular_file(configured, error))
            {
                return configured.wstring();
            }

            throw ExecutionException(Localization::WSLCCLI_DeveloperClusterAksEdgeNotFound());
        }

        if (const auto configured = GetEnvironmentValue(c_aksEdgeEnvironment); !configured.empty())
        {
            candidates.emplace_back(configured);
        }

        const auto executableDirectory = GetExecutableDirectory();
        candidates.emplace_back(executableDirectory / L"aksedge-windows-amd64.exe");
        candidates.emplace_back(executableDirectory / L"aksedge.exe");

        for (const auto& candidate : candidates)
        {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error))
            {
                return candidate.wstring();
            }
        }

        if (const auto onPath = FindOnPath(L"aksedge.exe"); !onPath.empty())
        {
            return onPath;
        }

        throw ExecutionException(Localization::WSLCCLI_DeveloperClusterAksEdgeNotFound());
    }

    std::wstring ResolveAgentDeb(ArgMap& args)
    {
        std::vector<std::filesystem::path> candidates;
        if (args.Contains(ArgType::DeveloperAgentDeb))
        {
            const std::filesystem::path configured{args.GetValue<ArgType::DeveloperAgentDeb>()};
            std::error_code error;
            return std::filesystem::is_regular_file(configured, error) ? configured.wstring() : std::wstring{};
        }

        if (const auto configured = GetEnvironmentValue(c_agentDebEnvironment); !configured.empty())
        {
            candidates.emplace_back(configured);
        }

        candidates.emplace_back(GetExecutableDirectory() / L"aks-bmagent.deb");
        for (const auto& candidate : candidates)
        {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error))
            {
                return candidate.wstring();
            }
        }

        return {};
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
            ADD_DEVELOPER_VALUE(DeveloperDistro);
            ADD_DEVELOPER_VALUE(DeveloperHostName);
            ADD_DEVELOPER_VALUE(DeveloperNodeName);
            ADD_DEVELOPER_VALUE(DeveloperNodeIp);
            ADD_DEVELOPER_VALUE(DeveloperApiPort);
            ADD_DEVELOPER_VALUE(DeveloperKubernetesVersion);
            ADD_DEVELOPER_VALUE(DeveloperPodCidr);
            ADD_DEVELOPER_VALUE(DeveloperDistribution);
            ADD_DEVELOPER_VALUE(DeveloperCni);
            ADD_DEVELOPER_VALUE(DeveloperGpuVendor);
            ADD_DEVELOPER_VALUE(DeveloperNetwork);
            ADD_DEVELOPER_VALUE(DeveloperKubeconfigOutput);
            ADD_DEVELOPER_VALUE(DeveloperMergeInto);
            ADD_DEVELOPER_VALUE(DeveloperAgentRepo);
            ADD_DEVELOPER_VALUE(DeveloperDiagnosticsOutput);
            ADD_DEVELOPER_VALUE(DeveloperDiagnosticsSince);
            ADD_DEVELOPER_VALUE(DeveloperPruneKubeconfigFile);
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
        case ArgType::DeveloperEnableGpu:
            present = args.GetValue<ArgType::DeveloperEnableGpu>();
            break;
        case ArgType::DeveloperMerge:
            present = args.GetValue<ArgType::DeveloperMerge>();
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

    std::wstring BuildAksEdgeCommandLine(const std::vector<std::wstring>& arguments)
    {
        std::vector<std::wstring_view> views;
        views.reserve(arguments.size());
        for (const auto& argument : arguments)
        {
            views.emplace_back(argument);
        }

        return wil::ArgvToCommandLine(views);
    }

    DWORD RunAksEdge(CLIExecutionContext& context, std::vector<std::wstring> arguments)
    {
        const auto executable = ResolveAksEdge(context.Args);
        arguments.insert(arguments.begin(), executable);
        const auto commandLine = BuildAksEdgeCommandLine(arguments);

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
            context.Terminal.Warn(L"\n{}\n", Localization::WSLCCLI_DeveloperClusterCancelCleanup());
            if (WaitForSingleObject(processHandle.get(), 10'000) == WAIT_TIMEOUT)
            {
                THROW_IF_WIN32_BOOL_FALSE(TerminateJobObject(job.get(), ERROR_CANCELLED));
            }
            THROW_HR(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        }

        THROW_LAST_ERROR_IF(wait != WAIT_OBJECT_0);
        return SubProcess::GetExitCode(processHandle.get());
    }

    void AddTargetArguments(std::vector<std::wstring>& command, ArgMap& args)
    {
        AddValue(command, args, ArgType::DeveloperName, L"--name");
        AddValue(command, args, ArgType::DeveloperDistro, L"--distro");
    }

} // namespace

std::vector<std::unique_ptr<Command>> DeveloperClusterCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"create", FullName(), DeveloperClusterOperation::Create));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(
        L"delete", FullName(), DeveloperClusterOperation::Delete, std::vector<std::wstring_view>{L"remove", L"rm"}));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"status", FullName(), DeveloperClusterOperation::Status));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"kubeconfig", FullName(), DeveloperClusterOperation::Kubeconfig));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"diagnostics", FullName(), DeveloperClusterOperation::Diagnostics));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"versions", FullName(), DeveloperClusterOperation::Versions));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"distributions", FullName(), DeveloperClusterOperation::Distributions));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"cnis", FullName(), DeveloperClusterOperation::Cnis));
    return commands;
}

std::wstring DeveloperClusterCommand::ShortDescription() const
{
    return Localization::WSLCCLI_DeveloperClusterCommandDesc();
}

std::wstring DeveloperClusterCommand::LongDescription() const
{
    return Localization::WSLCCLI_DeveloperClusterCommandLongDesc();
}

void DeveloperClusterCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp(context.Terminal);
}

std::vector<Argument> DeveloperClusterActionCommand::GetArguments() const
{
    std::vector<Argument> arguments{Argument::Create(ArgType::DeveloperAksEdgePath)};
    switch (m_operation)
    {
    case DeveloperClusterOperation::Create:
        arguments.insert(
            arguments.end(),
            {Argument::Create(ArgType::DeveloperName, true),
             Argument::Create(ArgType::DeveloperDistro),
             Argument::Create(ArgType::DeveloperHostName),
             Argument::Create(ArgType::DeveloperNodeName),
             Argument::Create(ArgType::DeveloperNodeIp),
             Argument::Create(ArgType::DeveloperAgentDeb),
             Argument::Create(ArgType::DeveloperAgentRepo),
             Argument::Create(ArgType::DeveloperApiPort),
             Argument::Create(ArgType::DeveloperKubernetesVersion),
             Argument::Create(ArgType::DeveloperPodCidr),
             Argument::Create(ArgType::DeveloperDistribution),
             Argument::Create(ArgType::DeveloperCni),
             Argument::Create(ArgType::DeveloperEnableGpu),
             Argument::Create(ArgType::DeveloperGpuVendor),
             Argument::Create(ArgType::DeveloperNetwork),
             Argument::Create(ArgType::DeveloperKubeconfigOutput),
             Argument::Create(ArgType::DeveloperMerge),
             Argument::Create(ArgType::DeveloperMergeInto)});
        break;
    case DeveloperClusterOperation::Delete:
        arguments.insert(
            arguments.end(),
            {Argument::Create(ArgType::DeveloperName), Argument::Create(ArgType::DeveloperDistro), Argument::Create(ArgType::DeveloperPruneKubeconfigFile)});
        break;
    case DeveloperClusterOperation::Status:
        arguments.push_back(Argument::Create(ArgType::DeveloperDistro));
        break;
    case DeveloperClusterOperation::Kubeconfig:
        arguments.insert(
            arguments.end(),
            {Argument::Create(ArgType::DeveloperName, true),
             Argument::Create(ArgType::DeveloperDistro),
             Argument::Create(ArgType::DeveloperKubeconfigOutput),
             Argument::Create(ArgType::DeveloperMerge),
             Argument::Create(ArgType::DeveloperMergeInto)});
        break;
    case DeveloperClusterOperation::Diagnostics:
        arguments.insert(
            arguments.end(),
            {Argument::Create(ArgType::DeveloperName),
             Argument::Create(ArgType::DeveloperDistro),
             Argument::Create(ArgType::DeveloperDiagnosticsOutput),
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

void DeveloperClusterActionCommand::ValidateArgumentsInternal(ArgMap& args) const
{
    if (args.GetValue<ArgType::DeveloperMerge>() && args.Contains(ArgType::DeveloperMergeInto))
    {
        throw ArgumentException(
            Localization::WSLCCLI_DeveloperClusterMergeConflict(),
            GetArgumentsForHelp({ArgType::DeveloperMerge, ArgType::DeveloperMergeInto}));
    }

    if (m_operation == DeveloperClusterOperation::Create && args.Contains(ArgType::DeveloperAgentDeb) && args.Contains(ArgType::DeveloperAgentRepo))
    {
        throw ArgumentException(
            Localization::WSLCCLI_DeveloperClusterAgentConflict(),
            GetArgumentsForHelp({ArgType::DeveloperAgentDeb, ArgType::DeveloperAgentRepo}));
    }

    if (m_operation == DeveloperClusterOperation::Create && !args.Contains(ArgType::DeveloperAgentRepo) && ResolveAgentDeb(args).empty())
    {
        throw ArgumentException(
            Localization::WSLCCLI_DeveloperClusterAgentNotFound(),
            GetArgumentsForHelp({ArgType::DeveloperAgentDeb, ArgType::DeveloperAgentRepo}));
    }
}

void DeveloperClusterActionCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    std::vector<std::wstring> command;
    switch (m_operation)
    {
    case DeveloperClusterOperation::Create:
    {
        command = {L"create", L"--driver", L"wsl"};
        AddTargetArguments(command, context.Args);
        AddValue(command, context.Args, ArgType::DeveloperHostName, L"--host-name");
        AddValue(command, context.Args, ArgType::DeveloperNodeName, L"--node-name");
        AddValue(command, context.Args, ArgType::DeveloperNodeIp, L"--node-ip");
        AddValue(command, context.Args, ArgType::DeveloperApiPort, L"--api-port");
        AddValue(command, context.Args, ArgType::DeveloperKubernetesVersion, L"--k8s-version");
        AddValue(command, context.Args, ArgType::DeveloperPodCidr, L"--pod-cidr");
        AddValue(command, context.Args, ArgType::DeveloperDistribution, L"--distribution");
        AddValue(command, context.Args, ArgType::DeveloperCni, L"--cni");
        AddValue(command, context.Args, ArgType::DeveloperGpuVendor, L"--gpu-vendor");
        AddValue(command, context.Args, ArgType::DeveloperNetwork, L"--network");
        AddValue(command, context.Args, ArgType::DeveloperKubeconfigOutput, L"--kubeconfig-out");
        AddValue(command, context.Args, ArgType::DeveloperMergeInto, L"--merge-into");
        AddFlag(command, context.Args, ArgType::DeveloperEnableGpu, L"--enable-gpu");
        AddFlag(command, context.Args, ArgType::DeveloperMerge, L"--merge");
        AddValue(command, context.Args, ArgType::DeveloperAgentRepo, L"--agent-repo");
        if (!context.Args.Contains(ArgType::DeveloperAgentRepo))
        {
            command.emplace_back(L"--agent-deb");
            command.emplace_back(ResolveAgentDeb(context.Args));
        }
        break;
    }
    case DeveloperClusterOperation::Delete:
        command = {L"delete", L"--driver", L"wsl", L"--purge", L"--prune-kubeconfig"};
        AddTargetArguments(command, context.Args);
        AddValue(command, context.Args, ArgType::DeveloperPruneKubeconfigFile, L"--prune-kubeconfig-file");
        break;
    case DeveloperClusterOperation::Status:
        command = {L"status", L"--driver", L"wsl"};
        AddValue(command, context.Args, ArgType::DeveloperDistro, L"--distro");
        break;
    case DeveloperClusterOperation::Kubeconfig:
        command = {L"kubeconfig", L"--driver", L"wsl"};
        AddTargetArguments(command, context.Args);
        AddValue(command, context.Args, ArgType::DeveloperKubeconfigOutput, L"--kubeconfig-out");
        AddValue(command, context.Args, ArgType::DeveloperMergeInto, L"--merge-into");
        AddFlag(command, context.Args, ArgType::DeveloperMerge, L"--merge");
        break;
    case DeveloperClusterOperation::Diagnostics:
        command = {L"diagnostics", L"--driver", L"wsl"};
        AddTargetArguments(command, context.Args);
        AddValue(command, context.Args, ArgType::DeveloperDiagnosticsOutput, L"--out");
        AddValue(command, context.Args, ArgType::DeveloperDiagnosticsSince, L"--since");
        AddFlag(command, context.Args, ArgType::DeveloperRedact, L"--redact");
        break;
    case DeveloperClusterOperation::Versions:
        command = {L"versions"};
        break;
    case DeveloperClusterOperation::Distributions:
        command = {L"distributions"};
        break;
    case DeveloperClusterOperation::Cnis:
        command = {L"cnis"};
        break;
    }

    const auto exitCode = RunAksEdge(context, std::move(command));
    if (exitCode != 0)
    {
        context.ExitCode = static_cast<int>(exitCode);
    }
}

std::wstring DeveloperClusterActionCommand::ShortDescription() const
{
    switch (m_operation)
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

std::wstring DeveloperClusterActionCommand::LongDescription() const
{
    switch (m_operation)
    {
    case DeveloperClusterOperation::Create:
        return Localization::WSLCCLI_DeveloperClusterCreateLongDesc();
    case DeveloperClusterOperation::Delete:
        return Localization::WSLCCLI_DeveloperClusterDeleteLongDesc();
    case DeveloperClusterOperation::Status:
        return Localization::WSLCCLI_DeveloperClusterStatusLongDesc();
    case DeveloperClusterOperation::Kubeconfig:
        return Localization::WSLCCLI_DeveloperClusterKubeconfigLongDesc();
    case DeveloperClusterOperation::Diagnostics:
        return Localization::WSLCCLI_DeveloperClusterDiagnosticsLongDesc();
    case DeveloperClusterOperation::Versions:
        return Localization::WSLCCLI_DeveloperClusterVersionsLongDesc();
    case DeveloperClusterOperation::Distributions:
        return Localization::WSLCCLI_DeveloperClusterDistributionsLongDesc();
    case DeveloperClusterOperation::Cnis:
        return Localization::WSLCCLI_DeveloperClusterCnisLongDesc();
    }

    THROW_HR(E_UNEXPECTED);
}

} // namespace wsl::windows::wslc
