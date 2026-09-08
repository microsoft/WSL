/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ClusterCommand.cpp

Abstract:

    AKS Arc cluster lifecycle orchestration for a dedicated WSL distro.

--*/
#include "precomp.h"
#include "ArgumentConvertedTypes.h"
#include "ClusterCommand.h"
#include "DeveloperClusterCommand.h"
#include "Exceptions.h"
#include "SubProcess.h"
#include "resource.h"
#include "wslutil.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <cwctype>

using namespace wsl::shared;
using namespace wsl::windows::common;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {
namespace {

    constexpr auto c_defaultDistro = L"aks-edge";
    constexpr auto c_defaultLocation = L"eastus";
    constexpr auto c_defaultDistribution = L"k8s";
    constexpr auto c_defaultAuthMode = L"browser";
    constexpr auto c_stage = "~/.aksarc-deploy";

    struct ClusterOptions
    {
        std::wstring Subscription;
        std::wstring ResourceGroup;
        std::wstring TenantId;
        std::wstring Location = c_defaultLocation;
        std::wstring Distribution = c_defaultDistribution;
        std::wstring Distro = c_defaultDistro;
        std::wstring AuthMode = c_defaultAuthMode;
        std::wstring ClientId;
        std::wstring ClientSecret;
        std::wstring CmpSubscription;
        std::wstring CmpResourceGroup;
        std::wstring CmpName;
        std::wstring Wheel;
        std::wstring BuildId;
        std::wstring Output;
        std::wstring Fleet;
        std::wstring FleetSubscription;
        std::wstring FleetResourceGroup;
        std::wstring FleetLocation;
        std::wstring FleetMemberName;
        bool EnableGpu = false;
        bool CreateFleet = false;
        bool MoveFleet = false;
        bool Yes = false;
    };

    std::wstring Trim(std::wstring value)
    {
        const auto first = value.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos)
        {
            return {};
        }

        const auto last = value.find_last_not_of(L" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    std::wstring Unquote(std::wstring value)
    {
        value = Trim(std::move(value));
        if (value.size() >= 2 && ((value.front() == L'\'' && value.back() == L'\'') || (value.front() == L'"' && value.back() == L'"')))
        {
            value = value.substr(1, value.size() - 2);
        }

        return value;
    }

    std::map<std::wstring, std::wstring> ReadConfig(const std::wstring& path)
    {
        std::map<std::wstring, std::wstring> values;
        if (path.empty())
        {
            return values;
        }

        std::ifstream input{std::filesystem::path(path), std::ios::binary};
        if (!input)
        {
            throw ExecutionException(std::format(L"Could not open cluster config file '{}'.", path));
        }

        std::string nativeLine;
        while (std::getline(input, nativeLine))
        {
            auto line = MultiByteToWide(nativeLine);
            line = Trim(std::move(line));
            if (line.empty() || line.front() == L'#')
            {
                continue;
            }

            const auto equals = line.find(L'=');
            if (equals == std::wstring::npos)
            {
                throw ExecutionException(std::format(L"Invalid cluster config line: '{}'. Expected KEY=VALUE.", line));
            }

            values.insert_or_assign(Trim(line.substr(0, equals)), Unquote(line.substr(equals + 1)));
        }

        return values;
    }

    template <ArgType Type>
    void Override(std::wstring& target, ArgMap& args)
    {
        if (args.Contains(Type))
        {
            target = args.GetValue<Type>();
        }
    }

    ClusterOptions GetOptions(ArgMap& args)
    {
        const auto configPath = args.Contains(ArgType::ClusterConfig) ? args.GetValue<ArgType::ClusterConfig>() : std::wstring{};
        const auto config = ReadConfig(configPath);
        const auto fromConfig = [&](const wchar_t* key, const wchar_t* defaultValue = L"") {
            const auto found = config.find(key);
            return found == config.end() ? std::wstring{defaultValue} : found->second;
        };

        ClusterOptions options{
            .Subscription = fromConfig(L"SUBSCRIPTION"),
            .ResourceGroup = fromConfig(L"RESOURCE_GROUP"),
            .TenantId = fromConfig(L"TENANT_ID"),
            .Location = fromConfig(L"LOCATION", c_defaultLocation),
            .Distribution = fromConfig(L"DISTRIBUTION", c_defaultDistribution),
            .Distro = fromConfig(L"DISTRO", c_defaultDistro),
            .AuthMode = fromConfig(L"AUTH_MODE", c_defaultAuthMode),
            .ClientId = fromConfig(L"AZURE_CLIENT_ID"),
            .ClientSecret = fromConfig(L"AZURE_CLIENT_SECRET"),
            .CmpSubscription = fromConfig(L"CMP_SUBSCRIPTION"),
            .CmpResourceGroup = fromConfig(L"CMP_RESOURCE_GROUP"),
            .CmpName = fromConfig(L"CMP_NAME"),
            .Wheel = fromConfig(L"AKSARC_WHEEL_PATH"),
            .BuildId = fromConfig(L"AKSARC_BUILD_ID"),
            .Output = fromConfig(L"KUBECONFIG_OUTPUT"),
            .Fleet = fromConfig(L"FLEET_NAME"),
            .FleetSubscription = fromConfig(L"FLEET_SUBSCRIPTION"),
            .FleetResourceGroup = fromConfig(L"FLEET_RESOURCE_GROUP"),
            .FleetLocation = fromConfig(L"FLEET_LOCATION"),
            .FleetMemberName = fromConfig(L"FLEET_MEMBER_NAME"),
            .EnableGpu = _wcsicmp(fromConfig(L"ENABLE_GPU", L"false").c_str(), L"true") == 0,
            .CreateFleet = _wcsicmp(fromConfig(L"CREATE_FLEET", L"false").c_str(), L"true") == 0,
        };

        Override<ArgType::ClusterSubscription>(options.Subscription, args);
        Override<ArgType::ClusterResourceGroup>(options.ResourceGroup, args);
        Override<ArgType::ClusterTenantId>(options.TenantId, args);
        Override<ArgType::ClusterLocation>(options.Location, args);
        Override<ArgType::ClusterDistribution>(options.Distribution, args);
        Override<ArgType::ClusterDistro>(options.Distro, args);
        Override<ArgType::ClusterAuthMode>(options.AuthMode, args);
        Override<ArgType::ClusterClientId>(options.ClientId, args);
        Override<ArgType::ClusterClientSecret>(options.ClientSecret, args);
        Override<ArgType::ClusterCmpSubscription>(options.CmpSubscription, args);
        Override<ArgType::ClusterCmpResourceGroup>(options.CmpResourceGroup, args);
        Override<ArgType::ClusterCmpName>(options.CmpName, args);
        Override<ArgType::ClusterWheel>(options.Wheel, args);
        Override<ArgType::ClusterBuildId>(options.BuildId, args);
        Override<ArgType::ClusterOutput>(options.Output, args);
        Override<ArgType::ClusterFleet>(options.Fleet, args);
        Override<ArgType::ClusterFleetSubscription>(options.FleetSubscription, args);
        Override<ArgType::ClusterFleetResourceGroup>(options.FleetResourceGroup, args);
        Override<ArgType::ClusterFleetLocation>(options.FleetLocation, args);
        Override<ArgType::ClusterFleetMemberName>(options.FleetMemberName, args);
        if (args.GetValue<ArgType::ClusterEnableGpu>())
        {
            options.EnableGpu = true;
        }
        if (args.GetValue<ArgType::ClusterFleetCreate>())
        {
            options.CreateFleet = true;
        }
        options.MoveFleet = args.GetValue<ArgType::ClusterFleetMove>();
        options.Yes = args.GetValue<ArgType::ClusterFleetYes>();

        if (options.FleetSubscription.empty())
        {
            options.FleetSubscription = options.Subscription;
        }
        if (options.FleetResourceGroup.empty())
        {
            options.FleetResourceGroup = options.ResourceGroup;
        }
        if (options.FleetLocation.empty())
        {
            options.FleetLocation = options.Location;
        }

        return options;
    }

    void Require(const std::wstring& value, std::wstring_view option)
    {
        if (value.empty())
        {
            throw ExecutionException(std::format(L"Missing {}. Supply it as a flag or in --config.", option));
        }
    }

    std::wstring BuildCommandLine(const std::vector<std::wstring>& arguments)
    {
        std::vector<std::wstring_view> views;
        views.reserve(arguments.size());
        for (const auto& argument : arguments)
        {
            views.emplace_back(argument);
        }

        return wil::ArgvToCommandLine(views);
    }

    DWORD RunProcess(const std::vector<std::wstring>& arguments)
    {
        const auto commandLine = BuildCommandLine(arguments);
        SubProcess process(nullptr, commandLine.c_str());
        return process.Run();
    }

    SubProcess::ProcessOutput CaptureProcess(const std::vector<std::wstring>& arguments)
    {
        const auto commandLine = BuildCommandLine(arguments);
        SubProcess process(nullptr, commandLine.c_str());
        return process.RunAndCaptureOutput();
    }

    std::vector<std::wstring> WslScriptArguments(const std::wstring& distro, const std::string& script)
    {
        const auto encoded = MultiByteToWide(Base64Encode(script));
        return {
            L"wsl.exe",
            L"-d",
            distro,
            L"-u",
            L"root",
            L"--",
            L"bash",
            L"-c",
            std::format(L"echo {} | base64 -d | bash -l", encoded),
        };
    }

    DWORD RunWslScript(const std::wstring& distro, const std::string& script)
    {
        return RunProcess(WslScriptArguments(distro, script));
    }

    SubProcess::ProcessOutput CaptureWslScript(const std::wstring& distro, const std::string& script)
    {
        return CaptureProcess(WslScriptArguments(distro, script));
    }

    DWORD RunWslScriptWithInput(const std::wstring& distro, const std::string& script, const std::string& input)
    {
        auto [read, write] = OpenAnonymousPipe(0, false, false);
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

        const std::vector<std::wstring> arguments{L"wsl.exe", L"-d", distro, L"-u", L"root", L"--", L"bash", L"-lc", MultiByteToWide(script)};
        const auto commandLine = BuildCommandLine(arguments);
        SubProcess process(nullptr, commandLine.c_str());
        process.SetStdHandles(read.get(), nullptr, nullptr);
        const auto processHandle = process.Start();
        read.reset();

        DWORD written = 0;
        THROW_IF_WIN32_BOOL_FALSE(WriteFile(write.get(), input.data(), gsl::narrow<DWORD>(input.size()), &written, nullptr));
        THROW_HR_IF(E_UNEXPECTED, written != input.size());
        write.reset();
        return SubProcess::GetExitCode(processHandle.get());
    }

    bool DistroExists(const std::wstring& distro)
    {
        return CaptureProcess({L"wsl.exe", L"-d", distro, L"--", L"true"}).ExitCode == 0;
    }

    std::string ShellQuote(const std::wstring& value)
    {
        auto utf8 = WideToMultiByte(value);
        size_t offset = 0;
        while ((offset = utf8.find('\'', offset)) != std::string::npos)
        {
            utf8.replace(offset, 1, "'\\''");
            offset += 4;
        }

        return "'" + utf8 + "'";
    }

    std::string SerializeConfig(const ClusterOptions& options)
    {
        return std::format(
            "SUBSCRIPTION={}\nRESOURCE_GROUP={}\nTENANT_ID={}\nLOCATION={}\nDISTRIBUTION={}\n"
            "ENABLE_GPU={}\nAUTH_MODE={}\nAZURE_CLIENT_ID={}\nAZURE_CLIENT_SECRET={}\n"
            "CMP_SUBSCRIPTION={}\nCMP_RESOURCE_GROUP={}\nCMP_NAME={}\nAKSARC_WHEEL_PATH={}\nAKSARC_BUILD_ID={}\n",
            ShellQuote(options.Subscription),
            ShellQuote(options.ResourceGroup),
            ShellQuote(options.TenantId),
            ShellQuote(options.Location),
            ShellQuote(options.Distribution),
            options.EnableGpu ? "'true'" : "'false'",
            ShellQuote(options.AuthMode),
            ShellQuote(options.ClientId),
            ShellQuote(options.ClientSecret),
            ShellQuote(options.CmpSubscription),
            ShellQuote(options.CmpResourceGroup),
            ShellQuote(options.CmpName),
            ShellQuote(options.Wheel),
            ShellQuote(options.BuildId));
    }

    std::filesystem::path ExtractScript(WORD resourceId, std::wstring_view fileName)
    {
        const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
        THROW_LAST_ERROR_IF(resource == nullptr);
        const auto loaded = LoadResource(nullptr, resource);
        THROW_LAST_ERROR_IF(loaded == nullptr);
        const auto data = LockResource(loaded);
        THROW_LAST_ERROR_IF(data == nullptr);
        const auto size = SizeofResource(nullptr, resource);
        THROW_LAST_ERROR_IF(size == 0);

        auto path = std::filesystem::temp_directory_path() / std::format(L"wslc-aksarc-{}-{}", GetCurrentProcessId(), fileName);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        THROW_HR_IF(E_FAIL, !output);
        output.write(static_cast<const char*>(data), size);
        THROW_HR_IF(E_FAIL, !output);
        return path;
    }

    std::string ToWslPath(const std::filesystem::path& path)
    {
        const auto native = path.wstring();
        THROW_HR_IF(E_INVALIDARG, native.size() < 3 || native[1] != L':');
        auto relative = WideToMultiByte(native.substr(3));
        std::ranges::replace(relative, '\\', '/');
        return std::format("/mnt/{}/{}", static_cast<char>(std::towlower(native[0])), relative);
    }

    void EnsureDistro(const ClusterOptions& options)
    {
        if (DistroExists(options.Distro))
        {
            return;
        }

        const auto result = RunProcess({L"wsl.exe", L"--install", L"Ubuntu-24.04", L"--name", options.Distro, L"--no-launch"});
        if (result != 0 || !DistroExists(options.Distro))
        {
            throw ExecutionException(std::format(L"Failed to create WSL distro '{}'.", options.Distro));
        }
    }

    void Stage(const ClusterOptions& options)
    {
        const auto deployPath = ExtractScript(IDR_AKSARC_DEPLOY_SCRIPT, L"deploy.sh");
        const auto gpuPath = ExtractScript(IDR_AKSARC_GPU_SCRIPT, L"gpu.sh");
        const auto removeScripts = wil::scope_exit([&]() {
            std::error_code ignored;
            std::filesystem::remove(deployPath, ignored);
            std::filesystem::remove(gpuPath, ignored);
        });
        const auto stageScript = std::format(
            "set -e; mkdir -p {0}; cp {1} {0}/setup-aks-arc-deploy.sh; "
            "cp {2} {0}/enable-gpu-wsl.sh; "
            "sed -i 's/\\r$//' {0}/setup-aks-arc-deploy.sh {0}/enable-gpu-wsl.sh; "
            "chmod 700 {0}/setup-aks-arc-deploy.sh {0}/enable-gpu-wsl.sh",
            c_stage,
            "'" + ToWslPath(deployPath) + "'",
            "'" + ToWslPath(gpuPath) + "'");
        if (RunWslScript(options.Distro, stageScript) != 0)
        {
            throw ExecutionException(L"Failed to stage the AKS Arc deployment script.");
        }

        if (RunWslScriptWithInput(options.Distro, std::format("umask 077; cat > {}/deploy-config.env", c_stage), SerializeConfig(options)) != 0)
        {
            throw ExecutionException(L"Failed to stage the AKS Arc deployment configuration.");
        }
    }

    std::filesystem::path KeepAliveLockPath(const std::wstring& distro)
    {
        auto safe = distro;
        std::ranges::replace_if(safe, [](wchar_t value) { return !std::iswalnum(value) && value != L'-' && value != L'_'; }, L'-');
        return std::filesystem::temp_directory_path() / std::format(L"wslc-keepalive-{}.pid", safe);
    }

    bool IsWslProcess(DWORD pid)
    {
        wil::unique_handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
        if (!process)
        {
            return false;
        }

        std::wstring image(32768, L'\0');
        DWORD size = gsl::narrow<DWORD>(image.size());
        if (!QueryFullProcessImageNameW(process.get(), 0, image.data(), &size))
        {
            return false;
        }

        image.resize(size);
        return _wcsicmp(std::filesystem::path(image).filename().c_str(), L"wsl.exe") == 0;
    }

    void StartKeepAlive(const std::wstring& distro)
    {
        const auto lockPath = KeepAliveLockPath(distro);
        {
            std::wifstream lock(lockPath);
            DWORD pid = 0;
            if (lock >> pid; pid != 0 && IsWslProcess(pid))
            {
                return;
            }
        }

        const std::vector<std::wstring> arguments{L"wsl.exe", L"-d", distro, L"--", L"sleep", L"infinity"};
        const auto commandLine = BuildCommandLine(arguments);
        wil::unique_hfile nullHandle{CreateFileW(
            L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        THROW_LAST_ERROR_IF(!nullHandle);
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(nullHandle.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

        SubProcess process(nullptr, commandLine.c_str(), CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP);
        process.SetStdHandles(nullHandle.get(), nullHandle.get(), nullHandle.get());
        const auto processHandle = process.Start();
        std::wofstream lock(lockPath, std::ios::trunc);
        THROW_HR_IF(E_FAIL, !lock);
        lock << GetProcessId(processHandle.get());
    }

    void StopKeepAlive(const std::wstring& distro)
    {
        const auto lockPath = KeepAliveLockPath(distro);
        DWORD pid = 0;
        {
            std::wifstream lock(lockPath);
            lock >> pid;
        }

        if (pid != 0 && IsWslProcess(pid))
        {
            wil::unique_handle process{OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)};
            if (process)
            {
                THROW_IF_WIN32_BOOL_FALSE(TerminateProcess(process.get(), 0));
                THROW_LAST_ERROR_IF(WaitForSingleObject(process.get(), 5000) == WAIT_FAILED);
            }
        }

        std::error_code ignored;
        std::filesystem::remove(lockPath, ignored);
    }

    std::vector<Argument> ConnectionArguments(bool includeTenant)
    {
        std::vector<Argument> arguments{
            Argument::Create(ArgType::ClusterConfig),
            Argument::Create(ArgType::ClusterSubscription),
            Argument::Create(ArgType::ClusterResourceGroup),
            Argument::Create(ArgType::ClusterDistro),
        };
        if (includeTenant)
        {
            arguments.push_back(Argument::Create(ArgType::ClusterTenantId));
        }
        return arguments;
    }

    std::vector<Argument> FleetArguments(bool includeCreateOptions)
    {
        std::vector<Argument> arguments{
            Argument::Create(ArgType::ClusterFleet),
            Argument::Create(ArgType::ClusterFleetSubscription),
            Argument::Create(ArgType::ClusterFleetResourceGroup),
            Argument::Create(ArgType::ClusterFleetMemberName),
        };
        if (includeCreateOptions)
        {
            arguments.push_back(Argument::Create(ArgType::ClusterFleetLocation));
            arguments.push_back(Argument::Create(ArgType::ClusterFleetCreate));
            arguments.push_back(Argument::Create(ArgType::ClusterFleetMove));
        }
        return arguments;
    }

    void AppendArguments(std::vector<Argument>& target, std::vector<Argument>&& source)
    {
        for (auto& argument : source)
        {
            if (std::ranges::none_of(target, [&](const auto& existing) { return existing.Type() == argument.Type(); }))
            {
                target.push_back(std::move(argument));
            }
        }
    }

    std::optional<ArgType> FindPresentArgument(const ArgMap& args, std::initializer_list<ArgType> types)
    {
        const auto found = std::ranges::find_if(types, [&](ArgType type) { return args.Contains(type); });
        return found == types.end() ? std::nullopt : std::optional{*found};
    }

    std::vector<Argument> RelatedArguments(std::initializer_list<ArgType> types)
    {
        std::vector<Argument> arguments;
        arguments.reserve(types.size());
        std::ranges::transform(types, std::back_inserter(arguments), [](ArgType type) { return Argument::Create(type); });
        return arguments;
    }

    void ValidateManagedArguments(ArgMap& args, std::initializer_list<ArgType> additionalDeveloperOptions = {})
    {
        const auto developerOption = FindPresentArgument(
            args,
            {ArgType::DeveloperAksEdgePath,
             ArgType::DeveloperAgentDeb,
             ArgType::DeveloperAgentRepo,
             ArgType::DeveloperApiPort,
             ArgType::DeveloperCni,
             ArgType::DeveloperDiagnosticsOutput,
             ArgType::DeveloperDiagnosticsSince,
             ArgType::DeveloperGpuVendor,
             ArgType::DeveloperHostName,
             ArgType::DeveloperKubernetesVersion,
             ArgType::DeveloperMerge,
             ArgType::DeveloperMergeInto,
             ArgType::DeveloperName,
             ArgType::DeveloperNetwork,
             ArgType::DeveloperNodeIp,
             ArgType::DeveloperNodeName,
             ArgType::DeveloperPodCidr,
             ArgType::DeveloperPruneKubeconfigFile,
             ArgType::DeveloperRedact});
        const auto offending = developerOption ? developerOption : FindPresentArgument(args, additionalDeveloperOptions);
        if (offending)
        {
            const auto argument = Argument::Create(*offending);
            throw ArgumentException(
                Localization::WSLCCLI_ClusterDeveloperOptionRequiresMode(argument.Name()),
                RelatedArguments({*offending, ArgType::ClusterDeveloper}));
        }
    }

    void ValidateDeveloperModeArguments(DeveloperClusterOperation operation, ArgMap& args)
    {
        const auto managedOption = FindPresentArgument(
            args,
            {ArgType::ClusterAuthMode,
             ArgType::ClusterBuildId,
             ArgType::ClusterClientId,
             ArgType::ClusterClientSecret,
             ArgType::ClusterCmpName,
             ArgType::ClusterCmpResourceGroup,
             ArgType::ClusterCmpSubscription,
             ArgType::ClusterConfig,
             ArgType::ClusterFleet,
             ArgType::ClusterFleetCreate,
             ArgType::ClusterFleetLocation,
             ArgType::ClusterFleetMemberName,
             ArgType::ClusterFleetMove,
             ArgType::ClusterFleetResourceGroup,
             ArgType::ClusterFleetSubscription,
             ArgType::ClusterFleetYes,
             ArgType::ClusterLocation,
             ArgType::ClusterResourceGroup,
             ArgType::ClusterSubscription,
             ArgType::ClusterTenantId,
             ArgType::ClusterWheel});
        if (managedOption)
        {
            const auto argument = Argument::Create(*managedOption);
            throw ArgumentException(
                Localization::WSLCCLI_ClusterManagedOptionWithDeveloper(argument.Name()),
                RelatedArguments({*managedOption, ArgType::ClusterDeveloper}));
        }

        ValidateDeveloperClusterArguments(operation, args);
    }

    void ValidateFleetOptions(ArgMap& args, bool requireFleet)
    {
        const bool hasFleet = args.Contains(ArgType::ClusterFleet);
        const bool mayHaveFleetInConfig = args.Contains(ArgType::ClusterConfig);
        if (requireFleet && !hasFleet && !mayHaveFleetInConfig)
        {
            throw ArgumentException(
                Localization::WSLCCLI_ClusterFleetRequired(),
                Argument::Create(ArgType::ClusterFleet));
        }

        if (!hasFleet && !mayHaveFleetInConfig &&
            FindPresentArgument(
                args,
                {ArgType::ClusterFleetCreate,
                 ArgType::ClusterFleetLocation,
                 ArgType::ClusterFleetMemberName,
                 ArgType::ClusterFleetMove,
                 ArgType::ClusterFleetResourceGroup,
                 ArgType::ClusterFleetSubscription}))
        {
            throw ArgumentException(
                Localization::WSLCCLI_ClusterFleetRequired(),
                Argument::Create(ArgType::ClusterFleet));
        }
    }

    struct FleetMembership
    {
        std::wstring Id;
        std::wstring MemberName;
        std::wstring FleetId;
    };

    struct FleetMembershipCoordinates
    {
        std::wstring Subscription;
        std::wstring ResourceGroup;
        std::wstring Fleet;
        std::wstring MemberName;
    };

    std::wstring ResourceName(std::wstring_view id)
    {
        const auto slash = id.find_last_of(L'/');
        return std::wstring{id.substr(slash == std::wstring_view::npos ? 0 : slash + 1)};
    }

    std::wstring ResourceIdSegment(std::wstring_view id, std::wstring_view segment)
    {
        const auto marker = std::format(L"/{}/", segment);
        const auto start = id.find(marker);
        if (start == std::wstring_view::npos)
        {
            return {};
        }

        const auto valueStart = start + marker.size();
        const auto end = id.find(L'/', valueStart);
        return std::wstring{id.substr(valueStart, end == std::wstring_view::npos ? id.size() - valueStart : end - valueStart)};
    }

    std::wstring ExpectedFleetId(const ClusterOptions& options)
    {
        return std::format(
            L"/subscriptions/{}/resourceGroups/{}/providers/Microsoft.ContainerService/fleets/{}",
            options.FleetSubscription,
            options.FleetResourceGroup,
            options.Fleet);
    }

    std::vector<FleetMembership> ParseMemberships(std::wstring_view output)
    {
        std::vector<FleetMembership> memberships;
        size_t offset = 0;
        while (offset < output.size())
        {
            const auto end = output.find_first_of(L"\r\n", offset);
            auto line = Trim(std::wstring{output.substr(offset, end == std::wstring_view::npos ? output.size() - offset : end - offset)});
            offset = end == std::wstring_view::npos ? output.size() : output.find_first_not_of(L"\r\n", end);
            if (offset == std::wstring_view::npos)
            {
                offset = output.size();
            }
            if (line.empty())
            {
                continue;
            }

            const auto tab = line.find(L'\t');
            const auto id = tab == std::wstring::npos ? line : line.substr(0, tab);
            const auto marker = id.rfind(L"/members/");
            if (marker == std::wstring::npos)
            {
                continue;
            }

            memberships.push_back(
                {.Id = id,
                 .MemberName = tab == std::wstring::npos ? ResourceName(id) : Trim(line.substr(tab + 1)),
                 .FleetId = id.substr(0, marker)});
        }

        return memberships;
    }

    void EnsureFleetExtensions(const std::wstring& distro)
    {
        const auto script =
            "set -e; "
            "az extension add --name fleet --upgrade --only-show-errors >/dev/null; "
            "az extension add --name resource-graph --upgrade --only-show-errors >/dev/null";
        if (RunWslScript(distro, script) != 0)
        {
            throw ExecutionException(Localization::WSLCCLI_ClusterFleetExtensionFailed());
        }
    }

    std::wstring ResolveConnectedClusterId(const ClusterOptions& options)
    {
        const auto script = std::format(
            "set -e; az account set --subscription {0}; "
            "cluster=$(az aksarc list -g {1} --query '[0].name' -o tsv); "
            "[ -n \"$cluster\" ] || {{ echo 'AKS Arc cluster not found' >&2; exit 1; }}; "
            "for attempt in $(seq 1 30); do "
            "id=$(az connectedk8s show -g {1} -n \"$cluster\" --query id -o tsv 2>/dev/null || true); "
            "[ -n \"$id\" ] && {{ printf '%%s\\n' \"$id\"; exit 0; }}; sleep 10; done; "
            "echo 'Arc-connected Kubernetes resource not found' >&2; exit 1",
            ShellQuote(options.Subscription),
            ShellQuote(options.ResourceGroup));
        const auto result = CaptureWslScript(options.Distro, script);
        if (result.ExitCode != 0 || Trim(result.Stdout).empty())
        {
            throw ExecutionException(Localization::WSLCCLI_ClusterFleetConnectedClusterNotFound());
        }

        return Trim(result.Stdout);
    }

    std::wstring ResolveFleetId(const ClusterOptions& options)
    {
        const auto result = CaptureWslScript(
            options.Distro,
            std::format(
                "az fleet show --subscription {0} -g {1} -n {2} --query id -o tsv",
                ShellQuote(options.FleetSubscription),
                ShellQuote(options.FleetResourceGroup),
                ShellQuote(options.Fleet)));
        if (result.ExitCode == 0)
        {
            return Trim(result.Stdout);
        }

        const auto error = result.Stderr + result.Stdout;
        if (error.find(L"ResourceNotFound") != std::wstring::npos || error.find(L"ResourceGroupNotFound") != std::wstring::npos)
        {
            return {};
        }

        throw ExecutionException(Localization::WSLCCLI_ClusterFleetLookupFailed(options.Fleet));
    }

    std::vector<FleetMembership> FindFleetMemberships(const ClusterOptions& options, const std::wstring& clusterId)
    {
        const auto result = CaptureWslScript(
            options.Distro,
            std::format(
                "az graph query -q \"Resources | where type =~ 'microsoft.containerservice/fleets/members' | "
                "where tostring(properties.clusterResourceId) =~ '{}' | project id, name\" "
                "--query 'data[].[id, name]' -o tsv",
                WideToMultiByte(clusterId)));
        if (result.ExitCode != 0)
        {
            throw ExecutionException(Localization::WSLCCLI_ClusterFleetMembershipLookupFailed());
        }

        return ParseMemberships(result.Stdout);
    }

    FleetMembershipCoordinates GetMembershipCoordinates(const FleetMembership& membership)
    {
        FleetMembershipCoordinates result{
            .Subscription = ResourceIdSegment(membership.Id, L"subscriptions"),
            .ResourceGroup = ResourceIdSegment(membership.Id, L"resourceGroups"),
            .Fleet = ResourceName(membership.FleetId),
            .MemberName = membership.MemberName};
        if (result.Subscription.empty() || result.ResourceGroup.empty() || result.Fleet.empty() || result.MemberName.empty())
        {
            throw ExecutionException(Localization::WSLCCLI_ClusterFleetInvalidMembershipId(membership.Id));
        }

        return result;
    }

    bool DeleteMembership(const ClusterOptions& options, const FleetMembershipCoordinates& membership)
    {
        return RunWslScript(
                   options.Distro,
                   std::format(
                       "az fleet member delete --subscription {0} -g {1} --fleet-name {2} -n {3} --yes --only-show-errors",
                       ShellQuote(membership.Subscription),
                       ShellQuote(membership.ResourceGroup),
                       ShellQuote(membership.Fleet),
                       ShellQuote(membership.MemberName))) == 0;
    }

    bool CreateMembership(
        const ClusterOptions& options,
        const std::wstring& fleetSubscription,
        const std::wstring& fleetResourceGroup,
        const std::wstring& fleet,
        const std::wstring& memberName,
        const std::wstring& clusterId)
    {
        return RunWslScript(
                   options.Distro,
                   std::format(
                       "az fleet member create --subscription {0} -g {1} --fleet-name {2} -n {3} "
                       "--member-cluster-id {4} --only-show-errors",
                       ShellQuote(fleetSubscription),
                       ShellQuote(fleetResourceGroup),
                       ShellQuote(fleet),
                       ShellQuote(memberName),
                       ShellQuote(clusterId))) == 0;
    }

    bool Confirm(Terminal& terminal, std::wstring_view prompt)
    {
        if (!terminal.IsInputInteractive())
        {
            return false;
        }

        auto response = Trim(terminal.PromptForLine(prompt));
        std::ranges::transform(response, response.begin(), [](wchar_t value) { return std::towlower(value); });
        return response == L"y" || response == L"yes";
    }

    std::wstring EnsureFleet(CLIExecutionContext& context, const ClusterOptions& options)
    {
        if (auto fleetId = ResolveFleetId(options); !fleetId.empty())
        {
            return fleetId;
        }

        if (!options.CreateFleet &&
            !Confirm(
                context.Terminal,
                Localization::WSLCCLI_ClusterFleetCreatePrompt(
                    options.Fleet, options.FleetSubscription, options.FleetResourceGroup, options.FleetLocation)))
        {
            throw ExecutionException(Localization::WSLCCLI_ClusterFleetCreateDeclined(options.Fleet));
        }

        const auto script = std::format(
            "set -e; "
            "az group show --subscription {0} -n {1} >/dev/null 2>&1 || "
            "az group create --subscription {0} -n {1} -l {2} --only-show-errors >/dev/null; "
            "az fleet create --subscription {0} -g {1} -n {3} -l {2} "
            "--enable-hub --enable-managed-identity --only-show-errors --query id -o tsv",
            ShellQuote(options.FleetSubscription),
            ShellQuote(options.FleetResourceGroup),
            ShellQuote(options.FleetLocation),
            ShellQuote(options.Fleet));
        const auto result = CaptureWslScript(options.Distro, script);
        if (result.ExitCode != 0 || Trim(result.Stdout).empty())
        {
            throw ExecutionException(Localization::WSLCCLI_ClusterFleetCreateFailed(options.Fleet));
        }

        return Trim(result.Stdout);
    }

    void JoinFleet(CLIExecutionContext& context, const ClusterOptions& options)
    {
        EnsureFleetExtensions(options.Distro);
        const auto clusterId = ResolveConnectedClusterId(options);
        const auto memberships = FindFleetMemberships(options, clusterId);
        const auto expectedFleetId = ExpectedFleetId(options);
        for (const auto& membership : memberships)
        {
            if (_wcsicmp(membership.FleetId.c_str(), expectedFleetId.c_str()) == 0)
            {
                context.Terminal.Info(
                    L"{}", Localization::WSLCCLI_ClusterFleetAlreadyJoined(options.Fleet, membership.MemberName));
                return;
            }

            if (!options.MoveFleet)
            {
                throw ExecutionException(
                    Localization::WSLCCLI_ClusterFleetAlreadyMember(ResourceName(membership.FleetId)));
            }
        }

        std::vector<FleetMembershipCoordinates> membershipCoordinates;
        membershipCoordinates.reserve(memberships.size());
        std::ranges::transform(memberships, std::back_inserter(membershipCoordinates), GetMembershipCoordinates);

        EnsureFleet(context, options);
        size_t removedMemberships = 0;
        for (const auto& membership : membershipCoordinates)
        {
            if (!DeleteMembership(options, membership))
            {
                bool restored = true;
                for (size_t index = 0; index < removedMemberships; ++index)
                {
                    const auto& removed = membershipCoordinates[index];
                    restored &=
                        CreateMembership(
                            options,
                            removed.Subscription,
                            removed.ResourceGroup,
                            removed.Fleet,
                            removed.MemberName,
                            clusterId);
                }
                if (!restored)
                {
                    throw ExecutionException(Localization::WSLCCLI_ClusterFleetMoveRollbackFailed(options.Fleet));
                }
                throw ExecutionException(Localization::WSLCCLI_ClusterFleetDetachFailed());
            }
            ++removedMemberships;
        }

        const auto memberName = options.FleetMemberName.empty() ? ResourceName(clusterId) : options.FleetMemberName;
        if (!CreateMembership(
                options,
                options.FleetSubscription,
                options.FleetResourceGroup,
                options.Fleet,
                memberName,
                clusterId))
        {
            bool restored = true;
            for (const auto& membership : membershipCoordinates)
            {
                restored &=
                    CreateMembership(
                        options,
                        membership.Subscription,
                        membership.ResourceGroup,
                        membership.Fleet,
                        membership.MemberName,
                        clusterId);
            }
            if (!restored)
            {
                throw ExecutionException(Localization::WSLCCLI_ClusterFleetMoveRollbackFailed(options.Fleet));
            }
            throw ExecutionException(Localization::WSLCCLI_ClusterFleetJoinFailed(options.Fleet));
        }

        context.Terminal.Info(L"{}", Localization::WSLCCLI_ClusterFleetJoined(options.Fleet, memberName));
    }

} // namespace

std::vector<Argument> ClusterCommand::GetArguments() const
{
    return {};
}

std::vector<std::unique_ptr<Command>> ClusterCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<ClusterCreateCommand>(FullName()));
    commands.push_back(std::make_unique<ClusterDeleteCommand>(FullName()));
    commands.push_back(std::make_unique<ClusterStatusCommand>(FullName()));
    commands.push_back(std::make_unique<ClusterKubeconfigCommand>(FullName()));
    commands.push_back(std::make_unique<ClusterFleetCommand>(FullName()));
    commands.push_back(
        std::make_unique<DeveloperClusterActionCommand>(L"diagnostics", FullName(), DeveloperClusterOperation::Diagnostics));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"versions", FullName(), DeveloperClusterOperation::Versions));
    commands.push_back(
        std::make_unique<DeveloperClusterActionCommand>(L"distributions", FullName(), DeveloperClusterOperation::Distributions));
    commands.push_back(std::make_unique<DeveloperClusterActionCommand>(L"cnis", FullName(), DeveloperClusterOperation::Cnis));
    return commands;
}

std::wstring ClusterCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ClusterCommandDesc();
}
std::wstring ClusterCommand::LongDescription() const
{
    return Localization::WSLCCLI_ClusterCommandLongDesc();
}
void ClusterCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp(context.Terminal);
}

std::vector<Argument> ClusterCreateCommand::GetArguments() const
{
    std::vector<Argument> arguments{
        Argument::Create(ArgType::ClusterConfig),
        Argument::Create(ArgType::ClusterSubscription),
        Argument::Create(ArgType::ClusterResourceGroup),
        Argument::Create(ArgType::ClusterTenantId),
        Argument::Create(ArgType::ClusterLocation),
        Argument::Create(ArgType::ClusterDistribution),
        Argument::Create(ArgType::ClusterDistro),
        Argument::Create(ArgType::ClusterAuthMode),
        Argument::Create(ArgType::ClusterClientId),
        Argument::Create(ArgType::ClusterClientSecret),
        Argument::Create(ArgType::ClusterCmpSubscription),
        Argument::Create(ArgType::ClusterCmpResourceGroup),
        Argument::Create(ArgType::ClusterCmpName),
        Argument::Create(ArgType::ClusterWheel),
        Argument::Create(ArgType::ClusterBuildId),
        Argument::Create(ArgType::ClusterEnableGpu),
    };
    AppendArguments(arguments, GetDeveloperClusterArguments(DeveloperClusterOperation::Create));
    AppendArguments(arguments, FleetArguments(true));
    return arguments;
}

void ClusterCreateCommand::ValidateArgumentsInternal(ArgMap& args) const
{
    if (args.GetValue<ArgType::ClusterDeveloper>())
    {
        ValidateDeveloperModeArguments(DeveloperClusterOperation::Create, args);
        return;
    }

    ValidateManagedArguments(args, {ArgType::ClusterOutput});
    ValidateFleetOptions(args, false);
    if (args.Contains(ArgType::ClusterAuthMode) && args.GetValue<ArgType::ClusterAuthMode>() == L"sp" &&
        (!args.Contains(ArgType::ClusterConfig) && (!args.Contains(ArgType::ClusterClientId) || !args.Contains(ArgType::ClusterClientSecret))))
    {
        throw ArgumentException(L"--auth-mode sp requires --client-id and --client-secret, or a config file containing them.");
    }
}

std::wstring ClusterCreateCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ClusterCreateDesc();
}
std::wstring ClusterCreateCommand::LongDescription() const
{
    return Localization::WSLCCLI_ClusterCreateLongDesc();
}

void ClusterCreateCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    if (context.Args.GetValue<ArgType::ClusterDeveloper>())
    {
        ExecuteDeveloperCluster(DeveloperClusterOperation::Create, context);
        return;
    }

    const auto options = GetOptions(context.Args);
    if (options.Fleet.empty() && options.CreateFleet)
    {
        throw ExecutionException(Localization::WSLCCLI_ClusterFleetRequired());
    }
    Require(options.Subscription, L"--subscription");
    Require(options.ResourceGroup, L"--resource-group");
    Require(options.TenantId, L"--tenant-id");
    Require(options.Distro, L"--distro");
    if (options.AuthMode != L"browser" && options.AuthMode != L"device-code" && options.AuthMode != L"sp")
    {
        throw ExecutionException(L"--auth-mode must be browser, device-code, or sp.");
    }
    if (options.Distribution != L"k8s" && options.Distribution != L"k3s")
    {
        throw ExecutionException(L"--distribution must be k8s or k3s.");
    }
    if (options.Distribution == L"k3s")
    {
        Require(options.CmpSubscription, L"--cmp-subscription");
        Require(options.CmpResourceGroup, L"--cmp-resource-group");
        Require(options.CmpName, L"--cmp-name");
        if (options.Wheel.empty() && options.BuildId.empty())
        {
            throw ExecutionException(L"K3s requires --aksarc-wheel or --aksarc-build-id.");
        }
    }
    if (options.AuthMode == L"sp")
    {
        Require(options.ClientId, L"--client-id");
        Require(options.ClientSecret, L"--client-secret");
    }

    EnsureDistro(options);
    Stage(options);
    if (RunWslScript(options.Distro, std::format("cd {0} && ./setup-aks-arc-deploy.sh --phase prep --config {0}/deploy-config.env", c_stage)) != 0)
    {
        throw ExecutionException(L"AKS Arc preparation failed.");
    }

    if (RunProcess({L"wsl.exe", L"--terminate", options.Distro}) != 0)
    {
        throw ExecutionException(L"Failed to restart the cluster distro after enabling systemd.");
    }

    const auto pid1 =
        CaptureProcess({L"wsl.exe", L"-d", options.Distro, L"-u", L"root", L"--", L"ps", L"-p", L"1", L"-o", L"comm="});
    if (pid1.ExitCode != 0 || Trim(pid1.Stdout) != L"systemd")
    {
        throw ExecutionException(L"systemd is not PID 1 after restarting the cluster distro.");
    }

    StartKeepAlive(options.Distro);
    if (RunWslScript(options.Distro, std::format("cd {0} && ./setup-aks-arc-deploy.sh --phase deploy --config {0}/deploy-config.env", c_stage)) != 0)
    {
        throw ExecutionException(L"AKS Arc deployment failed.");
    }

    if (!options.Fleet.empty())
    {
        JoinFleet(context, options);
    }
}

std::vector<Argument> ClusterDeleteCommand::GetArguments() const
{
    auto arguments = ConnectionArguments(false);
    AppendArguments(arguments, GetDeveloperClusterArguments(DeveloperClusterOperation::Delete));
    return arguments;
}
std::wstring ClusterDeleteCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ClusterDeleteDesc();
}
std::wstring ClusterDeleteCommand::LongDescription() const
{
    return Localization::WSLCCLI_ClusterDeleteLongDesc();
}

void ClusterDeleteCommand::ValidateArgumentsInternal(ArgMap& args) const
{
    if (args.GetValue<ArgType::ClusterDeveloper>())
    {
        ValidateDeveloperModeArguments(DeveloperClusterOperation::Delete, args);
        return;
    }

    ValidateManagedArguments(args);
}

void ClusterDeleteCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    if (context.Args.GetValue<ArgType::ClusterDeveloper>())
    {
        ExecuteDeveloperCluster(DeveloperClusterOperation::Delete, context);
        return;
    }

    const auto options = GetOptions(context.Args);
    Require(options.Subscription, L"--subscription");
    Require(options.ResourceGroup, L"--resource-group");
    if (!DistroExists(options.Distro))
    {
        throw ExecutionException(std::format(L"WSL distro '{}' does not exist.", options.Distro));
    }

    const auto cleanup = std::format(
        "set -e; az account set --subscription {0}; "
        "machine=$(az connectedmachine list -g {1} --query '[0].name' -o tsv 2>/dev/null); "
        "if [ -z \"$machine\" ]; then "
        "remaining=$(az resource list -g {1} --query 'length(@)' -o tsv 2>/dev/null || echo 1); "
        "if [ \"$remaining\" != 0 ]; then echo 'ERROR: no Arc machine was found but the resource group is not empty' >&2; exit "
        "1; fi; "
        "else "
        "az aksarc undeploy -g {1} --arc-machine-names \"$machine\" --yes || "
        "az aksarc undeploy -g {1} --arc-machine-names \"$machine\" --yes; "
        "fi; "
        "token=$(az account get-access-token --resource https://management.azure.com/ --query accessToken -o tsv 2>/dev/null || "
        "true); "
        "if command -v azcmagent >/dev/null && [ -n \"$token\" ]; then "
        "azcmagent disconnect --access-token \"$token\" || azcmagent disconnect --force-local-only; fi",
        ShellQuote(options.Subscription),
        ShellQuote(options.ResourceGroup));
    if (RunWslScript(options.Distro, cleanup) != 0)
    {
        throw ExecutionException(L"AKS Arc cleanup failed; the distro was retained so cleanup can be retried.");
    }

    StopKeepAlive(options.Distro);
    if (RunProcess({L"wsl.exe", L"--unregister", options.Distro}) != 0)
    {
        throw ExecutionException(std::format(L"Failed to unregister WSL distro '{}'.", options.Distro));
    }
}

std::vector<Argument> ClusterStatusCommand::GetArguments() const
{
    auto arguments = ConnectionArguments(false);
    AppendArguments(arguments, GetDeveloperClusterArguments(DeveloperClusterOperation::Status));
    return arguments;
}
std::wstring ClusterStatusCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ClusterStatusDesc();
}
std::wstring ClusterStatusCommand::LongDescription() const
{
    return Localization::WSLCCLI_ClusterStatusLongDesc();
}

void ClusterStatusCommand::ValidateArgumentsInternal(ArgMap& args) const
{
    if (args.GetValue<ArgType::ClusterDeveloper>())
    {
        ValidateDeveloperModeArguments(DeveloperClusterOperation::Status, args);
        return;
    }

    ValidateManagedArguments(args);
}

void ClusterStatusCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    if (context.Args.GetValue<ArgType::ClusterDeveloper>())
    {
        ExecuteDeveloperCluster(DeveloperClusterOperation::Status, context);
        return;
    }

    const auto options = GetOptions(context.Args);
    if (!DistroExists(options.Distro))
    {
        context.Terminal.Output(L"distro '{}': not present\n", options.Distro);
        return;
    }

    StartKeepAlive(options.Distro);
    auto script = std::string{
        "echo '== distro =='; ps -p 1 -o comm= | sed 's/^/init: /'; "
        "echo '== arc =='; azcmagent show 2>/dev/null | grep -E 'Agent Status|Resource Name' || echo 'not connected'"};
    if (!options.ResourceGroup.empty())
    {
        script += std::format(
            "; echo '== cluster =='; az aksarc list -g {} --query '[].{{name:name,state:provisioningState}}' -o table",
            ShellQuote(options.ResourceGroup));
    }

    context.ExitCode = static_cast<int>(RunWslScript(options.Distro, script));
}

std::vector<Argument> ClusterKubeconfigCommand::GetArguments() const
{
    auto arguments = ConnectionArguments(false);
    arguments.push_back(Argument::Create(ArgType::ClusterOutput));
    AppendArguments(arguments, GetDeveloperClusterArguments(DeveloperClusterOperation::Kubeconfig));
    return arguments;
}
std::wstring ClusterKubeconfigCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ClusterKubeconfigDesc();
}
std::wstring ClusterKubeconfigCommand::LongDescription() const
{
    return Localization::WSLCCLI_ClusterKubeconfigLongDesc();
}

void ClusterKubeconfigCommand::ValidateArgumentsInternal(ArgMap& args) const
{
    if (args.GetValue<ArgType::ClusterDeveloper>())
    {
        ValidateDeveloperModeArguments(DeveloperClusterOperation::Kubeconfig, args);
        return;
    }

    ValidateManagedArguments(args);
}

void ClusterKubeconfigCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    if (context.Args.GetValue<ArgType::ClusterDeveloper>())
    {
        ExecuteDeveloperCluster(DeveloperClusterOperation::Kubeconfig, context);
        return;
    }

    const auto options = GetOptions(context.Args);
    if (!DistroExists(options.Distro))
    {
        throw ExecutionException(std::format(L"WSL distro '{}' does not exist.", options.Distro));
    }

    StartKeepAlive(options.Distro);
    auto script = std::string{"if [ -r /etc/kubernetes/admin.conf ]; then cat /etc/kubernetes/admin.conf; exit 0; fi; "};
    if (!options.Subscription.empty() && !options.ResourceGroup.empty())
    {
        script += std::format(
            "az account set --subscription {0}; name=$(az aksarc list -g {1} --query '[0].name' -o tsv); "
            "az aksarc get-credentials -g {1} -n \"$name\" --file /tmp/wslc-kubeconfig >/dev/null; cat /tmp/wslc-kubeconfig",
            ShellQuote(options.Subscription),
            ShellQuote(options.ResourceGroup));
    }
    else
    {
        script += "echo 'admin.conf is missing; --subscription and --resource-group are required for the fallback' >&2; exit 1";
    }

    const auto result = CaptureWslScript(options.Distro, script);
    if (result.ExitCode != 0)
    {
        throw ExecutionException(L"Failed to retrieve the cluster kubeconfig.");
    }

    if (options.Output.empty())
    {
        context.Terminal.Output(L"{}", result.Stdout);
        return;
    }

    std::ofstream output(std::filesystem::path(options.Output), std::ios::binary | std::ios::trunc);
    THROW_HR_IF(E_FAIL, !output);
    const auto utf8 = WideToMultiByte(result.Stdout);
    output.write(utf8.data(), utf8.size());
    THROW_HR_IF(E_FAIL, !output);
    context.Terminal.Info(L"Wrote kubeconfig to '{}'.\n", options.Output);
}

std::vector<Argument> ClusterFleetCommand::GetArguments() const
{
    return {};
}

std::vector<std::unique_ptr<Command>> ClusterFleetCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<ClusterFleetJoinCommand>(FullName()));
    commands.push_back(std::make_unique<ClusterFleetLeaveCommand>(FullName()));
    commands.push_back(std::make_unique<ClusterFleetStatusCommand>(FullName()));
    return commands;
}

std::wstring ClusterFleetCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ClusterFleetCommandDesc();
}

std::wstring ClusterFleetCommand::LongDescription() const
{
    return Localization::WSLCCLI_ClusterFleetCommandLongDesc();
}

void ClusterFleetCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp(context.Terminal);
}

std::vector<Argument> ClusterFleetJoinCommand::GetArguments() const
{
    auto arguments = ConnectionArguments(false);
    AppendArguments(arguments, FleetArguments(true));
    return arguments;
}

void ClusterFleetJoinCommand::ValidateArgumentsInternal(ArgMap& args) const
{
    ValidateFleetOptions(args, true);
}

std::wstring ClusterFleetJoinCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ClusterFleetJoinDesc();
}

std::wstring ClusterFleetJoinCommand::LongDescription() const
{
    return Localization::WSLCCLI_ClusterFleetJoinLongDesc();
}

void ClusterFleetJoinCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    const auto options = GetOptions(context.Args);
    Require(options.Subscription, L"--subscription");
    Require(options.ResourceGroup, L"--resource-group");
    Require(options.Distro, L"--distro");
    Require(options.Fleet, L"--fleet");
    Require(options.FleetSubscription, L"--fleet-subscription");
    Require(options.FleetResourceGroup, L"--fleet-resource-group");
    JoinFleet(context, options);
}

std::vector<Argument> ClusterFleetLeaveCommand::GetArguments() const
{
    auto arguments = ConnectionArguments(false);
    arguments.push_back(Argument::Create(ArgType::ClusterFleetYes));
    return arguments;
}

std::wstring ClusterFleetLeaveCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ClusterFleetLeaveDesc();
}

std::wstring ClusterFleetLeaveCommand::LongDescription() const
{
    return Localization::WSLCCLI_ClusterFleetLeaveLongDesc();
}

void ClusterFleetLeaveCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    const auto options = GetOptions(context.Args);
    Require(options.Subscription, L"--subscription");
    Require(options.ResourceGroup, L"--resource-group");
    Require(options.Distro, L"--distro");

    EnsureFleetExtensions(options.Distro);
    const auto clusterId = ResolveConnectedClusterId(options);
    const auto memberships = FindFleetMemberships(options, clusterId);
    if (memberships.empty())
    {
        context.Terminal.Output(L"{}", Localization::WSLCCLI_ClusterFleetNotMember(ResourceName(clusterId)));
        return;
    }
    if (memberships.size() != 1)
    {
        throw ExecutionException(Localization::WSLCCLI_ClusterFleetMultipleMemberships());
    }

    const auto& membership = memberships.front();
    const auto fleetName = ResourceName(membership.FleetId);
    if (!options.Yes &&
        !Confirm(
            context.Terminal,
            Localization::WSLCCLI_ClusterFleetLeavePrompt(ResourceName(clusterId), fleetName)))
    {
        throw ExecutionException(Localization::WSLCCLI_ClusterFleetLeaveDeclined());
    }

    if (!DeleteMembership(options, GetMembershipCoordinates(membership)))
    {
        throw ExecutionException(Localization::WSLCCLI_ClusterFleetDetachFailed());
    }

    context.Terminal.Info(L"{}", Localization::WSLCCLI_ClusterFleetLeft(fleetName));
}

std::vector<Argument> ClusterFleetStatusCommand::GetArguments() const
{
    return ConnectionArguments(false);
}

std::wstring ClusterFleetStatusCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ClusterFleetStatusDesc();
}

std::wstring ClusterFleetStatusCommand::LongDescription() const
{
    return Localization::WSLCCLI_ClusterFleetStatusLongDesc();
}

void ClusterFleetStatusCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    const auto options = GetOptions(context.Args);
    Require(options.Subscription, L"--subscription");
    Require(options.ResourceGroup, L"--resource-group");
    Require(options.Distro, L"--distro");

    EnsureFleetExtensions(options.Distro);
    const auto clusterId = ResolveConnectedClusterId(options);
    const auto memberships = FindFleetMemberships(options, clusterId);
    if (memberships.empty())
    {
        context.Terminal.Output(L"{}", Localization::WSLCCLI_ClusterFleetNotMember(ResourceName(clusterId)));
        return;
    }

    for (const auto& membership : memberships)
    {
        context.Terminal.Output(
            L"{}",
            Localization::WSLCCLI_ClusterFleetStatusOutput(
                ResourceName(clusterId), ResourceName(membership.FleetId), membership.MemberName, membership.Id));
    }
}

} // namespace wsl::windows::wslc
