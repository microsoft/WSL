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
        bool EnableGpu = false;
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
            .EnableGpu = _wcsicmp(fromConfig(L"ENABLE_GPU", L"false").c_str(), L"true") == 0,
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
        if (args.GetValue<ArgType::ClusterEnableGpu>())
        {
            options.EnableGpu = true;
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
    return {
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
}

void ClusterCreateCommand::ValidateArgumentsInternal(ArgMap& args) const
{
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
    const auto options = GetOptions(context.Args);
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
}

std::vector<Argument> ClusterDeleteCommand::GetArguments() const
{
    return ConnectionArguments(false);
}
std::wstring ClusterDeleteCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ClusterDeleteDesc();
}
std::wstring ClusterDeleteCommand::LongDescription() const
{
    return Localization::WSLCCLI_ClusterDeleteLongDesc();
}

void ClusterDeleteCommand::ExecuteInternal(CLIExecutionContext& context) const
{
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
    return ConnectionArguments(false);
}
std::wstring ClusterStatusCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ClusterStatusDesc();
}
std::wstring ClusterStatusCommand::LongDescription() const
{
    return Localization::WSLCCLI_ClusterStatusLongDesc();
}

void ClusterStatusCommand::ExecuteInternal(CLIExecutionContext& context) const
{
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

void ClusterKubeconfigCommand::ExecuteInternal(CLIExecutionContext& context) const
{
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

} // namespace wsl::windows::wslc
