/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslClient.cpp

Abstract:

    This file contains the logic for WSL client entry points.

--*/

#include "precomp.h"
#include "WslInstall.h"
#include "HandleConsoleProgressBar.h"
#include "Distribution.h"
#include "CommandLine.h"
#include <conio.h>

#define BASH_PATH L"/bin/bash"

using winrt::Windows::Foundation::Uri;
using winrt::Windows::Management::Deployment::DeploymentOptions;
using wsl::shared::Localization;
using wsl::windows::common::ClientExecutionContext;
using wsl::windows::common::Context;
using namespace wsl::windows::common;
using namespace wsl::shared;
using namespace wsl::windows::common::distribution;

static bool g_promptBeforeExit = false;

namespace {

enum Entrypoint
{
    Bash,
    Wsl,
    Wslconfig,
    Wslg
};

struct LaunchProcessOptions
{
    std::wstring CurrentWorkingDirectory;
    std::optional<GUID> DistroGuid;
    std::wstring Username;
    ULONG LaunchFlags = LXSS_LAUNCH_FLAG_ENABLE_INTEROP | LXSS_LAUNCH_FLAG_TRANSLATE_ENVIRONMENT;
};

struct ListOptions
{
    bool verbose;
    bool quiet;
    bool running;
    bool all;
    bool online;
};

struct ShellExecOptions
{
    std::optional<bool> UseShell;
    std::optional<bool> Login;

    bool DefaultUseShell = true;
    bool DefaultLogin = false;

    bool IsLogin() const
    {
        return Login.value_or(DefaultLogin);
    }

    bool IsUseShell() const
    {
        return UseShell.value_or(DefaultUseShell);
    }

    void SetExecMode()
    {
        UseShell = false;
        Login = false;
    }

    void ParseShellOptionArg(std::wstring_view Argument)
    {
        if (Argument == WSL_SHELL_OPTION_ARG_LOGIN_OPTION)
        {
            UseShell = true;
            Login = true;
        }
        else if (Argument == WSL_SHELL_OPTION_ARG_NOSHELL_OPTION)
        {
            SetExecMode();
        }
        else if (Argument == WSL_SHELL_OPTION_ARG_STANDARD_OPTION)
        {
            UseShell = true;
            Login = false;
        }
        else
        {
            THROW_HR(E_INVALIDARG);
        }
    }
};

bool IsInteractiveConsole()
{
    const HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode{};

    return GetFileType(stdinHandle) == FILE_TYPE_CHAR && GetConsoleMode(stdinHandle, &mode);
}

void PromptForKeyPress()
{
    if (IsInteractiveConsole())
    {
        wsl::windows::common::wslutil::PrintMessage(wsl::shared::Localization::MessagePressAnyKeyToExit());
        LOG_IF_WIN32_BOOL_FALSE(FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE)));
        _getch();
    }
}

// Forward function declarations.
bool InstallPrerequisites(_In_ bool installWslOptionalComponent);
int LaunchProcess(_In_opt_ LPCWSTR filename, _In_ int argc, _In_reads_(argc) LPCWSTR argv[], _In_ const LaunchProcessOptions& options);
int ListDistributionsHelper(_In_ ListOptions options);
LaunchProcessOptions ParseLegacyArguments(_Inout_ std::wstring_view& commandLine);
DWORD ParseVersionString(_In_ const std::wstring_view& versionString);
int SetSparse(GUID& distroGuid, bool sparse, bool allowUnsafe);
int Version();

template <typename T>
struct WslVersion
{
    T& value;

    int operator()(LPCWSTR Input) const
    {
        if (Input == nullptr)
        {
            return -1;
        }

        value = ParseVersionString(Input);
        return 1;
    }
};

// Function definitions.
int BashMain(_In_ std::wstring_view commandLine)
{
    // Call the MSI package if we're in an MSIX context
    if (wsl::windows::common::wslutil::IsRunningInMsix())
    {
        return wsl::windows::common::wslutil::CallMsiPackage();
    }

    const auto options = ParseLegacyArguments(commandLine);

    // If the command line is empty, construct the arguments in the following
    // format to launch bash as a login shell:
    //
    //     filename = /bin/bash
    //     argv[0]  = -bash
    //
    // N.B. This is the same logic that login uses to launch the shell.
    //
    // For non-empty command lines, construct the arguments in the following
    // format:
    //
    //     filename = /bin/bash
    //     argv[0]  = /bin/bash
    //     argv[1]  = -c
    //     argv[2]  = /bin/bash -c "commandLine"
    //
    // N.B. The arguments are set up this way to leave /bin/bash in charge of
    //      all argument parsing.
    int argc = 1;
    LPCWSTR argv[3];
    std::wstring arguments;
    LPCWSTR filename;
    if (commandLine.empty())
    {
        argv[0] = L"-bash";
        filename = BASH_PATH;
    }
    else
    {
        argc = RTL_NUMBER_OF(argv);
        arguments = BASH_PATH L" ";
        arguments.append(commandLine);
        argv[0] = BASH_PATH;
        argv[1] = L"-c";
        argv[2] = arguments.c_str();
        filename = argv[0];
    }

    return LaunchProcess(filename, argc, argv, options);
}

void ChangeDirectory(_In_ std::wstring_view argument, _Inout_ LaunchProcessOptions& options)
{
    std::wstring directory(wsl::windows::common::string::StripQuotes(argument));
    THROW_HR_IF(E_INVALIDARG, directory.empty());

    // There are two supported directory arguments:
    // 1. Any path that begins with a '/' or `~` is assumed to be a Linux path.
    //    If the path does not exist an error is logged to /dev/kmsg.
    // 2. Everything else is assumed to be a valid absolute Windows path.
    if ((directory[0] == L'/') || (directory[0] == L'~'))
    {
        options.CurrentWorkingDirectory = std::move(directory);
    }
    else
    {
        THROW_HR_IF(E_INVALIDARG, !std::filesystem::path(directory).is_absolute());

        THROW_IF_WIN32_BOOL_FALSE(SetCurrentDirectoryW(directory.c_str()));
    }
}

int ExportDistribution(_In_ std::wstring_view commandLine)
{
    ULONG flags = 0;
    ArgumentParser parser(std::wstring{commandLine}, WSL_BINARY_NAME);
    std::filesystem::path filePath;
    LPCWSTR name{};

    auto parseFormat = [&flags](LPCWSTR Value) {
        if (Value == nullptr)
        {
            return -1;
        }

        if (wsl::shared::string::IsEqual(L"tar.gz", Value))
        {
            WI_SetFlag(flags, LXSS_EXPORT_DISTRO_FLAGS_GZIP);
        }
        else if (wsl::shared::string::IsEqual(L"tar.xz", Value))
        {
            WI_SetFlag(flags, LXSS_EXPORT_DISTRO_FLAGS_XZIP);
        }
        else if (wsl::shared::string::IsEqual(L"vhd", Value))
        {
            WI_SetFlag(flags, LXSS_EXPORT_DISTRO_FLAGS_VHD);
        }
        else if (!wsl::shared::string::IsEqual(L"tar", Value))
        {
            THROW_HR(E_INVALIDARG);
        }

        return 1;
    };

    parser.AddPositionalArgument(name, 0);
    parser.AddPositionalArgument(filePath, 1);
    parser.AddArgument(SetFlag<ULONG, LXSS_EXPORT_DISTRO_FLAGS_VHD>(flags), WSL_EXPORT_ARG_VHD_OPTION);
    parser.AddArgument(parseFormat, WSL_EXPORT_ARG_FORMAT_OPTION);
    parser.Parse();

    THROW_HR_IF(
        WSL_E_INVALID_USAGE,
        filePath.empty() || (WI_IsFlagSet(flags, LXSS_EXPORT_DISTRO_FLAGS_GZIP) && WI_IsFlagSet(flags, LXSS_EXPORT_DISTRO_FLAGS_VHD)));

    // Determine if the target is stdout, or an on-disk file.
    wil::unique_hfile file;
    HANDLE fileHandle;
    if (filePath.wstring() == WSL_EXPORT_ARG_STDOUT)
    {
        fileHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    else
    {
        file.reset(CreateFileW(
            filePath.c_str(), GENERIC_WRITE, (FILE_SHARE_READ | FILE_SHARE_DELETE), nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));

        THROW_LAST_ERROR_IF(!file);

        fileHandle = file.get();
    }

    // Delete the target if export was unsuccessful.
    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        if (file)
        {
            LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(filePath.c_str()));
        }
    });

    // Export the distribution.
    wsl::windows::common::SvcComm service;
    const GUID distroId = service.GetDistributionId(name);

    {
        using wsl::windows::common::HandleConsoleProgressBar;

        HandleConsoleProgressBar exportProgress(fileHandle, Localization::MessageExportProgress(), HandleConsoleProgressBar::Format::FileSize);
        THROW_IF_FAILED(service.ExportDistribution(&distroId, fileHandle, flags));
    }

    if (file)
    {
        wsl::windows::common::wslutil::PrintSystemError(ERROR_SUCCESS);
    }

    cleanup.release();
    return 0;
}

int ImportDistribution(_In_ std::wstring_view commandLine)
{
    ArgumentParser parser(std::wstring{commandLine}, WSL_BINARY_NAME);
    LPCWSTR name{};
    std::optional<std::wstring> installPath{};
    std::filesystem::path filePath;
    ULONG flags = LXSS_IMPORT_DISTRO_FLAGS_NO_OOBE;
    DWORD version = LXSS_WSL_VERSION_DEFAULT;

    parser.AddPositionalArgument(name, 0);
    parser.AddPositionalArgument(AbsolutePath(installPath), 1);
    parser.AddPositionalArgument(filePath, 2);
    parser.AddArgument(WslVersion(version), WSL_IMPORT_ARG_VERSION);
    parser.AddArgument(SetFlag<ULONG, LXSS_IMPORT_DISTRO_FLAGS_VHD>{flags}, WSL_IMPORT_ARG_VHD);

    parser.Parse();

    if (name == nullptr || !installPath.has_value() || filePath.empty())
    {
        THROW_HR(E_INVALIDARG);
    }

    // Ensure that the install path exists.
    bool directoryCreated = true;
    if (!CreateDirectoryW(installPath->c_str(), nullptr))
    {
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            directoryCreated = false;
        }
        else
        {
            THROW_LAST_ERROR_MSG("CreateDirectoryW");
        }
    }

    auto directory_cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [directoryCreated, &installPath]() {
        if (directoryCreated)
        {
            LOG_IF_WIN32_BOOL_FALSE(RemoveDirectory(installPath->c_str()));
        }
    });

    // Determine if the source of the tar file is stdin, or an on-disk file.
    wil::unique_hfile file;
    HANDLE fileHandle;
    if (filePath.wstring() == WSL_IMPORT_ARG_STDIN)
    {
        fileHandle = GetStdHandle(STD_INPUT_HANDLE);
    }
    else
    {
        if (WI_IsFlagClear(flags, LXSS_IMPORT_DISTRO_FLAGS_VHD))
        {
            // Fail if expecting a tar, but the file name has the .vhd or .vhdx extension.
            if (wsl::windows::common::string::IsPathComponentEqual(filePath.extension().native(), wsl::windows::common::wslutil::c_vhdFileExtension) ||
                wsl::windows::common::string::IsPathComponentEqual(filePath.extension().native(), wsl::windows::common::wslutil::c_vhdxFileExtension))
            {
                wsl::windows::common::wslutil::PrintMessage(wsl::shared::Localization::MessagePassVhdFlag());
                return -1;
            }
        }

        file.reset(CreateFileW(
            filePath.c_str(), GENERIC_READ, (FILE_SHARE_READ | FILE_SHARE_DELETE), nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));

        THROW_LAST_ERROR_IF(!file);

        fileHandle = file.get();
    }

    // Register the distribution.
    {
        wsl::windows::common::HandleConsoleProgressBar progressBar(fileHandle, Localization::MessageImportProgress());
        wsl::windows::common::SvcComm service;
        service.RegisterDistribution(name, version, fileHandle, installPath->c_str(), flags);
    }

    directory_cleanup.release();
    wsl::windows::common::wslutil::PrintSystemError(ERROR_SUCCESS);
    return 0;
}

int ImportDistributionInplace(_In_ std::wstring_view commandLine)
{
    // Parse the command line.
    int argc = 0;
    const wil::unique_hlocal_ptr<LPWSTR[]> argv{CommandLineToArgvW(std::wstring(commandLine).c_str(), &argc)};
    THROW_LAST_ERROR_IF(!argv);

    THROW_HR_IF(WSL_E_INVALID_USAGE, argc != 2);

    const auto name(argv[0]);
    const auto filePath = wsl::windows::common::filesystem::GetFullPath(argv[1]);

    wsl::windows::common::SvcComm service;
    service.ImportDistributionInplace(name, filePath.c_str());
    wsl::windows::common::wslutil::PrintSystemError(ERROR_SUCCESS);
    return 0;
}

int LaunchElevated(_In_ LPCWSTR commandLine)
{
    wsl::windows::common::wslutil::PrintMessage(
        wsl::windows::common::wslutil::GetSystemErrorString(HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED)));

    // Add the attach parent process argument to the command line and shell execute an elevated version of wsl.exe.
    std::wstring arguments;
    arguments += WSL_PARENT_CONSOLE_ARG L" ";
    arguments += std::to_wstring(GetCurrentProcessId());
    arguments += L" ";
    arguments += commandLine;

    const auto path = wil::GetModuleFileNameW<std::wstring>(wil::GetModuleInstanceHandle());
    SHELLEXECUTEINFOW execInfo{};
    execInfo.cbSize = sizeof(execInfo);
    execInfo.fMask = (SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE | SEE_MASK_FLAG_NO_UI);
    execInfo.lpFile = path.c_str();
    execInfo.lpVerb = L"runas";
    execInfo.nShow = SW_HIDE;
    execInfo.lpParameters = arguments.c_str();
    THROW_IF_WIN32_BOOL_FALSE(ShellExecuteExW(&execInfo));
    const wil::unique_handle process{execInfo.hProcess};

    // Get the process exit code.
    WI_VERIFY(WaitForSingleObject(process.get(), INFINITE) == WAIT_OBJECT_0);

    DWORD exitCode;
    THROW_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(process.get(), &exitCode));
    return static_cast<int>(exitCode);
}

int Install(_In_ std::wstring_view commandLine)
{

    // Parse options.
    std::optional<std::wstring> distroArgument;
    std::optional<std::wstring> fromFile;
    std::optional<std::wstring> name;
    std::optional<std::filesystem::path> location;
    std::optional<ULONG> version;
    std::optional<uint64_t> vhdSize;
    bool fixedVhd = false;
    bool installWslOptionalComponent = false;
    bool noLaunchAfterInstall = false;
    bool noDistribution = false;
    bool legacy = false;
    bool webDownload = IsWindowsServer();

    ArgumentParser parser(std::wstring{commandLine}, WSL_BINARY_NAME);
    parser.AddPositionalArgument(distroArgument, 0);
    parser.AddArgument(distroArgument, WSL_INSTALL_ARG_DIST_OPTION_LONG, WSL_INSTALL_ARG_DIST_OPTION);
    parser.AddArgument(noLaunchAfterInstall, WSL_INSTALL_ARG_NO_LAUNCH_OPTION_LONG, WSL_INSTALL_ARG_NO_LAUNCH_OPTION);
    parser.AddArgument(webDownload, WSL_INSTALL_ARG_WEB_DOWNLOAD_LONG);
    parser.AddArgument(noDistribution, WSL_INSTALL_ARG_NO_DISTRIBUTION_OPTION);
    parser.AddArgument(installWslOptionalComponent, WSL_INSTALL_ARG_ENABLE_WSL1_LONG);
    parser.AddArgument(NoOp{}, WSL_INSTALL_ARG_PRERELEASE_LONG); // Unused but handled because argument may be present when invoked from inbox.
    parser.AddArgument(fromFile, WSL_INSTALL_ARG_FROM_FILE_LONG, WSL_INSTALL_ARG_FROM_FILE_OPTION);
    parser.AddArgument(name, WSL_INSTALL_ARG_NAME_LONG);
    parser.AddArgument(AbsolutePath(location), WSL_INSTALL_ARG_LOCATION_LONG, WSL_INSTALL_ARG_LOCATION_OPTION);
    parser.AddArgument(legacy, WSL_INSTALL_ARG_LEGACY_LONG);
    parser.AddArgument(WslVersion(version), WSL_INSTALL_ARG_VERSION);
    parser.AddArgument(g_promptBeforeExit, WSL_INSTALL_ARG_PROMPT_BEFORE_EXIT_OPTION);
    parser.AddArgument(SizeString(vhdSize), WSL_INSTALL_ARG_VHD_SIZE);
    parser.AddArgument(fixedVhd, WSL_INSTALL_ARG_FIXED_VHD);

    parser.Parse();

    if (noDistribution && distroArgument.has_value())
    {
        THROW_HR_WITH_USER_ERROR(
            E_INVALIDARG, Localization::MessageArgumentsNotValidTogether(WSL_INSTALL_ARG_NO_DISTRIBUTION_OPTION, WSL_INSTALL_ARG_DIST_OPTION_LONG));
    }

    if (fixedVhd && !vhdSize.has_value())
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageArgumentNotValidWithout(WSL_INSTALL_ARG_FIXED_VHD, WSL_INSTALL_ARG_VHD_SIZE));
    }

    // A distribution to be installed can be specified in three ways:
    // wsl.exe --install --distribution Ubuntu
    // wsl.exe --install Ubuntu
    // wsl.exe --install
    //
    // N.B. The legacy method (specifying --distribution) is no longer documented,
    // but is still supported to avoid breaking existing scripts.
    if (fromFile.has_value())
    {
        if (distroArgument.has_value())
        {
            THROW_HR_WITH_USER_ERROR(
                E_INVALIDARG, Localization::MessageArgumentsNotValidTogether(WSL_INSTALL_ARG_FROM_FILE_LONG, WSL_INSTALL_ARG_DIST_OPTION_LONG));
        }

        wil::unique_hfile diskFile;
        HANDLE file{};
        if (fromFile.value() == WSL_IMPORT_ARG_STDIN)
        {
            file = GetStdHandle(STD_INPUT_HANDLE);
            fromFile = L"<stdin>";
        }
        else
        {
            diskFile.reset(CreateFileW(
                fromFile->c_str(), GENERIC_READ, (FILE_SHARE_READ | FILE_SHARE_DELETE), nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));

            THROW_LAST_ERROR_IF(!diskFile);

            file = diskFile.get();
        }

        wsl::windows::common::wslutil::PrintMessage(Localization::MessageInstalling(fromFile->c_str()));
        wsl::windows::common::HandleConsoleProgressBar progressBar(file, Localization::MessageImportProgress());

        SvcComm service;
        auto [id, installedName] = service.RegisterDistribution(
            name.has_value() ? name->c_str() : nullptr,
            version.value_or(LXSS_WSL_VERSION_DEFAULT),
            file,
            location.has_value() ? location->c_str() : nullptr,
            fixedVhd ? LXSS_IMPORT_DISTRO_FLAGS_FIXED_VHD : 0,
            vhdSize);

        wsl::windows::common::wslutil::PrintMessage(Localization::MessageDistributionInstalled(installedName.get()), stdout);

        if (!noLaunchAfterInstall)
        {
            wsl::windows::common::wslutil::PrintMessage(Localization::MessageLaunchingDistro(installedName.get()), stdout);

            LaunchProcessOptions options{};
            options.DistroGuid = id;
            return LaunchProcess(nullptr, 0, nullptr, options);
        }

        return 0;
    }

    bool rebootRequired = InstallPrerequisites(installWslOptionalComponent);
    if (rebootRequired)
    {
        noLaunchAfterInstall = false;
    }

    // Install a distribution only if no reboot is required, or if we're on the --legacy path (to maintain old behavior).
    const Distribution* legacyDistro = nullptr;

    WslInstall::InstallResult installResult{};
    if (!noDistribution && (legacy || !rebootRequired))
    {
        auto result = WslInstall::InstallDistribution(
            installResult, distroArgument, version, !noLaunchAfterInstall, webDownload, legacy, fixedVhd, name, location, vhdSize);

        std::optional<std::wstring> flavor;
        if (installResult.Distribution.has_value())
        {
            if (const auto* distro = std::get_if<ModernDistributionVersion>(&*installResult.Distribution))
            {
                flavor = distro->Name;
            }
            else
            {
                legacyDistro = std::get_if<Distribution>(&*installResult.Distribution);
                WI_ASSERT(legacyDistro != nullptr);

                flavor = legacyDistro->Name;
            }
        }

        // Logs when a specific distribution is installed, and whether that was successful. Used to report distro usage to distro maintainers
        WSL_LOG_TELEMETRY(
            "InstallDistribution",
            PDT_ProductAndServiceUsage,
            TraceLoggingValue(result, "result"),
            TraceLoggingValue(legacyDistro == nullptr, "modern"),
            TraceLoggingValue(flavor.value_or(L"<none>").c_str(), "flavor"));

        THROW_IF_FAILED(result);
    }

    if (rebootRequired)
    {
        wsl::windows::common::wslutil::PrintSystemError(ERROR_SUCCESS_REBOOT_REQUIRED);
    }
    else if (noDistribution)
    {
        wsl::windows::common::wslutil::PrintSystemError(NO_ERROR);
    }
    else
    {
        if (!installResult.Alreadyinstalled)
        {
            wsl::windows::common::wslutil::PrintMessage(Localization::MessageDistributionInstalled(installResult.Name));
        }

        if (!noLaunchAfterInstall)
        {
            wsl::windows::common::wslutil::PrintMessage(Localization::MessageLaunchingDistro(installResult.Name), stdout);

            if (legacyDistro != nullptr)
            {
                wsl::windows::common::distribution::Launch(*legacyDistro, installResult.InstalledViaGithub, !installResult.Alreadyinstalled);
            }
            else
            {
                LaunchProcessOptions options{};
                options.DistroGuid = installResult.Id.value();

                return LaunchProcess(nullptr, 0, nullptr, options);
            }
        }
    }

    return 0;
}

bool InstallPrerequisites(_In_ bool installWslOptionalComponent)
{
    const auto missingComponents = WslInstall::CheckForMissingOptionalComponents(installWslOptionalComponent);
    if (missingComponents.empty())
    {
        return false;
    }

    // Install any optional components that have not yet been installed.
    const auto token = wil::open_current_access_token();
    if (!wsl::windows::common::security::IsTokenElevated(token.get()))
    {
        const auto elevatedCommand = std::format(
            L"{} {} {}", WSL_INSTALL_ARG, WSL_INSTALL_ARG_NO_DISTRIBUTION_OPTION, installWslOptionalComponent ? WSL_INSTALL_ARG_ENABLE_WSL1_LONG : L"");

        const auto exitCode = LaunchElevated(elevatedCommand.c_str());
        if (exitCode != 0)
        {
            return exitCode;
        }
    }
    else
    {
        WslInstall::InstallOptionalComponents(missingComponents);
    }

    return true;
}

int LaunchProcess(_In_opt_ LPCWSTR filename, _In_ int argc, _In_reads_(argc) LPCWSTR argv[], _In_ const LaunchProcessOptions& options)
{
    // Create an instance of the specified distribution.
    //
    // N.B. If creating the instance fails because the file system needs to
    //      be upgraded, the appropriate message is displayed before
    //      re-attempting the create while allowing the upgrade. This is
    //      only done if running in interactive mode.
    const LPCGUID distribution = options.DistroGuid.has_value() ? &options.DistroGuid.value() : nullptr;
    wsl::windows::common::SvcComm service;
    if (argc == 0)
    {
        ClientExecutionContext context;
        const auto result = service.CreateInstanceNoThrow(distribution, 0, context.OutError());
        if (FAILED(result))
        {
            if (result == WSL_E_FS_UPGRADE_NEEDED)
            {
                wsl::windows::common::wslutil::PrintMessage(wsl::shared::Localization::MessageFsUpgradeNeeded(), stderr);
            }
            else
            {
                THROW_HR(result);
            }
        }
    }

    const int exitCode = service.LaunchProcess(
        distribution,
        filename,
        argc,
        argv,
        options.LaunchFlags,
        options.Username.empty() ? nullptr : options.Username.c_str(),
        options.CurrentWorkingDirectory.empty() ? nullptr : options.CurrentWorkingDirectory.c_str());

    THROW_HR_IF(WSL_E_USER_NOT_FOUND, (exitCode == LX_INIT_USER_NOT_FOUND));
    THROW_HR_IF(WSL_E_TTY_LIMIT, (exitCode == LX_INIT_TTY_LIMIT));

    return exitCode;
}

int ListDistributions(_In_ std::wstring_view commandLine)
{
    ListOptions options{};
    ArgumentParser parser(std::wstring{commandLine}, WSL_BINARY_NAME);
    parser.AddArgument(options.all, WSL_LIST_ARG_ALL_OPTION);
    parser.AddArgument(options.running, WSL_LIST_ARG_RUNNING_OPTION);
    parser.AddArgument(options.quiet, WSL_LIST_ARG_QUIET_OPTION_LONG, WSL_LIST_ARG_QUIET_OPTION);
    parser.AddArgument(options.verbose, WSL_LIST_ARG_VERBOSE_OPTION_LONG, WSL_LIST_ARG_VERBOSE_OPTION);
    parser.AddArgument(options.online, WSL_LIST_ARG_ONLINE_OPTION_LONG, WSL_LIST_ARG_ONLINE_OPTION);

    parser.Parse();

    return ListDistributionsHelper(options);
}

int ListDistributionsHelper(_In_ ListOptions options)
{
    // Handle invalid options.
    THROW_HR_IF(
        WSL_E_INVALID_USAGE,
        ((options.quiet && options.verbose) || (options.all && options.running)) || ((options.verbose || options.all) && options.online));

    // Query all registered distributions and sort the list so the default
    // (if present) is first.
    wsl::windows::common::SvcComm service;
    auto distros = service.EnumerateDistributions();
    std::sort(distros.begin(), distros.end(), [](const auto& Left, const auto&) {
        return (WI_IsFlagSet(Left.Flags, LXSS_ENUMERATE_FLAGS_DEFAULT));
    });

    if (options.verbose)
    {
        THROW_HR_IF(WSL_E_DEFAULT_DISTRO_NOT_FOUND, distros.empty());

        // Determine max length of a distro name and construct the format string.
        size_t maxLength = wcslen(WSL_LIST_HEADER_NAME);
        std::for_each(distros.begin(), distros.end(), [&](const auto& entry) {
            const size_t length = wcslen(entry.DistroName);
            if (length > maxLength)
            {
                maxLength = length;
            }
        });

        std::wstring formatString(L"%s %-");
        formatString += std::to_wstring(maxLength + 4);
        formatString += L"s%-16s%s\n";

        // Print distribution information.
        wprintf(formatString.c_str(), L" ", WSL_LIST_HEADER_NAME, WSL_LIST_HEADER_STATE, WSL_LIST_HEADER_VERSION);
        std::for_each(distros.begin(), distros.end(), [&](const auto& entry) {
            const LPCWSTR defaultDistro = WI_IsFlagSet(entry.Flags, LXSS_ENUMERATE_FLAGS_DEFAULT) ? L"*" : L" ";
            const std::wstring version(std::to_wstring(entry.Version));
            auto state = L"Stopped";
            switch (entry.State)
            {
            case LxssDistributionStateRunning:
                state = L"Running";
                break;

            case LxssDistributionStateInstalling:
                state = L"Installing";
                break;

            case LxssDistributionStateUninstalling:
                state = L"Uninstalling";
                break;

            case LxssDistributionStateConverting:
                state = L"Converting";
                break;

            case LxssDistributionStateExporting:
                state = L"Exporting";
                break;

            default:
                break;
            }

            wprintf(formatString.c_str(), defaultDistro, entry.DistroName, state, version.c_str());
        });
    }
    else if (!options.online)
    {
        if (options.running)
        {
            std::erase_if(distros, [&](const auto& entry) { return (entry.State != LxssDistributionStateRunning); });

            if ((!options.quiet) && (distros.empty()))
            {
                wsl::windows::common::wslutil::PrintMessage(wsl::shared::Localization::MessageNoRunningDistro());
                return -1;
            }
        }

        if (!options.all)
        {
            std::erase_if(distros, [&](const auto& entry) {
                return (
                    (entry.State == LxssDistributionStateInstalling) || (entry.State == LxssDistributionStateUninstalling) ||
                    (entry.State == LxssDistributionStateConverting) || (entry.State == LxssDistributionStateExporting));
            });
        }

        if (!options.quiet)
        {
            THROW_HR_IF(WSL_E_DEFAULT_DISTRO_NOT_FOUND, distros.empty());

            wsl::windows::common::wslutil::PrintMessage(wsl::shared::Localization::MessageRegisteredDistrosHeader());
        }

        std::for_each(distros.begin(), distros.end(), [&](const auto& entry) {
            if ((!options.quiet) && WI_IsFlagSet(entry.Flags, LXSS_ENUMERATE_FLAGS_DEFAULT))
            {
                wsl::windows::common::wslutil::PrintMessage(Localization::MessagePrintDistroDefault(entry.DistroName), stdout);
            }
            else
            {
                wprintf(L"%s\n", entry.DistroName);
            }
        });
    }
    else
    {
        std::vector<std::pair<std::wstring, std::wstring>> names;

        size_t maxLength = wcslen(WSL_LIST_HEADER_NAME);

        auto appendIfNotPresent = [&](const std::wstring& name, const std::wstring& friendlyName) {
            auto pred = [&name](const auto& e) { return e.first == name; };

            if (std::find_if(names.begin(), names.end(), pred) == names.end())
            {
                names.emplace_back(name, friendlyName);

                if (name.size() > maxLength)
                {
                    maxLength = name.size();
                }
            }
        };

        auto readNames = [&](const DistributionList& distributions) {
            if (distributions.ModernDistributions.has_value())
            {
                for (const auto& [name, versions] : *distributions.ModernDistributions)
                {
                    for (auto i = 0; i < versions.size(); i++)
                    {
                        if (!options.all && i > 3)
                        {
                            break; // Only show 3 entries per distro unless --all is passed.
                        }

                        appendIfNotPresent(versions[i].Name, versions[i].FriendlyName);
                    }
                }
            }

            if (distributions.Distributions.has_value())
            {
                for (const auto& e : *distributions.Distributions)
                {
                    appendIfNotPresent(e.Name, e.FriendlyName);
                }
            }
        };

        const auto manifest = wsl::windows::common::distribution::GetAvailable();
        if (manifest.OverrideManifest.has_value())
        {
            readNames(*manifest.OverrideManifest);
        }

        readNames(manifest.Manifest);

        std::wstring formatString(L"%-");
        formatString += std::to_wstring(maxLength + 4);
        formatString += L"s%s\n";

        wsl::windows::common::wslutil::PrintMessage(wsl::shared::Localization::MessageDistributionListOnline(WSL_INSTALL_ARG));
        wprintf(formatString.c_str(), WSL_LIST_HEADER_NAME, WSL_LIST_HEADER_FRIENDLY_NAME);
        std::for_each(names.begin(), names.end(), [&](const auto& entry) {
            wprintf(formatString.c_str(), entry.first.c_str(), entry.second.c_str());
        });
    }

    return 0;
}

int Manage(_In_ std::wstring_view commandLine)
{
    LPCWSTR distribution{};
    std::optional<bool> sparse;
    std::optional<std::wstring> move;
    std::optional<std::wstring> defaultUser;
    std::optional<uint64_t> resize;
    bool allowUnsafe = false;

    ArgumentParser parser(std::wstring{commandLine}, WSL_BINARY_NAME, 0);
    parser.AddPositionalArgument(distribution, 0);
    parser.AddArgument(ParsedBool(sparse), WSL_MANAGE_ARG_SET_SPARSE_OPTION_LONG, WSL_MANAGE_ARG_SET_SPARSE_OPTION);
    parser.AddArgument(AbsolutePath(move), WSL_MANAGE_ARG_MOVE_OPTION_LONG, WSL_MANAGE_ARG_MOVE_OPTION);
    parser.AddArgument(defaultUser, WSL_MANAGE_ARG_SET_DEFAULT_USER_OPTION_LONG);
    parser.AddArgument(SizeString(resize), WSL_MANAGE_ARG_RESIZE_OPTION_LONG, WSL_MANAGE_ARG_RESIZE_OPTION);
    parser.AddArgument(allowUnsafe, WSL_MANAGE_ARG_ALLOW_UNSAFE);
    parser.Parse();

    THROW_HR_IF(WSL_E_INVALID_USAGE, distribution == nullptr);

    wsl::windows::common::SvcComm service;
    auto distroGuid = service.GetDistributionId(distribution);

    if (sparse.has_value() + move.has_value() + defaultUser.has_value() + resize.has_value() != 1)
    {
        THROW_HR(WSL_E_INVALID_USAGE);
    }

    if (sparse)
    {
        SetSparse(distroGuid, sparse.value(), allowUnsafe);
    }
    else if (move)
    {
        service.MoveDistribution(distroGuid, move->c_str());
    }
    else if (defaultUser)
    {
        auto wslExe = wil::GetModuleFileNameW<std::wstring>(wil::GetModuleInstanceHandle());

        auto commandLine = std::format(
            L"\"{}\" {} -u root /usr/bin/id -u -- '{}'",
            wslExe,
            wsl::shared::string::GuidToString<wchar_t>(distroGuid),
            defaultUser.value());

        wsl::windows::common::SubProcess process{wslExe.c_str(), commandLine.c_str()};

        auto result = process.RunAndCaptureOutput(INFINITE, GetStdHandle(STD_ERROR_HANDLE));
        if (result.ExitCode != 0)
        {
            return result.ExitCode;
        }

        while (!result.Stdout.empty() && (result.Stdout.back() == '\r' || result.Stdout.back() == '\n'))
        {
            result.Stdout.pop_back();
        }

        wchar_t* endPtr{};
        auto newUid = std::wcstoul(result.Stdout.c_str(), &endPtr, 10);

        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), endPtr != result.Stdout.c_str() + result.Stdout.size());

        service.ConfigureDistribution(&distroGuid, newUid, LXSS_DISTRO_FLAGS_UNCHANGED);
    }
    else if (resize)
    {
        THROW_IF_FAILED(service.ResizeDistribution(&distroGuid, resize.value()));
    }

    wsl::windows::common::wslutil::PrintSystemError(ERROR_SUCCESS);
    return 0;
}

int Mount(_In_ std::wstring_view commandLine)
{
    bool vhd = false;
    bool bare = false;
    std::optional<std::wstring> options;
    ULONG partition = 0;
    std::optional<std::wstring> type;
    std::optional<std::wstring> name;
    std::wstring disk;

    ArgumentParser parser(std::wstring{commandLine}, WSL_BINARY_NAME);
    parser.AddArgument(bare, WSL_MOUNT_ARG_BARE_OPTION_LONG);
    parser.AddArgument(vhd, WSL_MOUNT_ARG_VHD_OPTION_LONG);
    parser.AddArgument(options, WSL_MOUNT_ARG_OPTIONS_OPTION_LONG, WSL_MOUNT_ARG_OPTIONS_OPTION);
    parser.AddArgument(Integer(partition), WSL_MOUNT_ARG_PARTITION_OPTION_LONG, WSL_MOUNT_ARG_PARTITION_OPTION);
    parser.AddArgument(type, WSL_MOUNT_ARG_TYPE_OPTION_LONG, WSL_MOUNT_ARG_TYPE_OPTION);
    parser.AddArgument(name, WSL_MOUNT_ARG_NAME_OPTION_LONG, WSL_MOUNT_ARG_NAME_OPTION);
    parser.AddPositionalArgument(UnquotedPath(disk), 0);
    parser.Parse();

    THROW_HR_IF(WSL_E_INVALID_USAGE, disk.empty());

    ULONG flags = 0;
    if (vhd)
    {
        WI_SetFlag(flags, LXSS_ATTACH_MOUNT_FLAGS_VHD);
        disk = wsl::windows::common::filesystem::GetFullPath(disk.c_str()).wstring();
    }
    else
    {
        WI_SetFlag(flags, LXSS_ATTACH_MOUNT_FLAGS_PASS_THROUGH);
    }

    // First attach the disk to the vm
    wsl::windows::common::SvcComm service;
    const auto result = service.AttachDisk(disk.c_str(), flags);
    if (FAILED(result))
    {
        THROW_HR_IF(result, bare);

        // In the case of a non-bare mount, WSL_E_DISK_ALREADY_ATTACHED and LXSS_E_USER_VHD_ALREADY_ATTACHED are
        // ok to ignore because the user can mount more than one partition on the same disk
        // (so that disk might be already attached).
        THROW_HR_IF(result, result != WSL_E_DISK_ALREADY_ATTACHED && result != WSL_E_USER_VHD_ALREADY_ATTACHED);
    }

    // Perform the mount
    if (!bare)
    {
        const auto mountResult = service.MountDisk(
            disk.c_str(),
            flags,
            partition,
            name.has_value() ? name->c_str() : nullptr,
            type.has_value() ? type->c_str() : nullptr,
            options.has_value() ? options->c_str() : nullptr);

        if (mountResult.Result != 0)
        {
            wsl::windows::common::wslutil::PrintMessage(
                Localization::MessageDiskMountFailed(strerror(-mountResult.Result), WSL_UNMOUNT_ARG, disk), stdout);
            return 1;
        }
        else
        {
            wsl::windows::common::wslutil::PrintMessage(
                Localization::MessageDiskMounted(mountResult.MountName.get(), WSL_UNMOUNT_ARG, disk), stdout);
        }
    }
    else
    {
        wsl::windows::common::wslutil::PrintSystemError(ERROR_SUCCESS);
    }

    return 0;
}

LaunchProcessOptions ParseLegacyArguments(_Inout_ std::wstring_view& commandLine)
{
    // Strip the executable name. Because this has to be a legal file name, quoted parts cannot contain escaped quotes.
    BOOLEAN inQuotes = FALSE;
    while ((!commandLine.empty()) && ((inQuotes != FALSE) || (!LXSS_IS_WHITESPACE(commandLine[0]))))
    {
        if (commandLine[0] == L'"')
        {
            inQuotes = !inQuotes;
        }

        commandLine = commandLine.substr(1);
    }

    // Strip any leading whitespace.
    commandLine = wsl::windows::common::string::StripLeadingWhitespace(commandLine);

    // Check for a distributon GUID as the first parameter and strip it out if present.
    auto argument = wsl::windows::common::helpers::ParseArgument(commandLine);
    auto distroGuid = wsl::shared::string::ToGuid(argument);
    if (distroGuid.has_value())
    {
        commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
    }

    // Check for the home directory parameter and strip it out if present.
    std::wstring currentWorkingDirectory;
    argument = wsl::windows::common::helpers::ParseArgument(commandLine);
    if (argument == WSL_CWD_HOME)
    {
        currentWorkingDirectory = WSL_CWD_HOME;
        commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
    }

    return {std::move(currentWorkingDirectory), std::move(distroGuid)};
}

DWORD
ParseVersionString(_In_ const std::wstring_view& versionString)
{
    DWORD version;
    const auto result = wil::ResultFromException([&]() { version = std::stoi(std::wstring(versionString)); });
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_VERSION_PARSE_ERROR), (FAILED(result) || ((version != LXSS_WSL_VERSION_1) && (version != LXSS_WSL_VERSION_2))));

    return version;
}

int SetDefaultDistribution(_In_ LPCWSTR distributionName)
{
    wsl::windows::common::SvcComm service;
    const GUID distroGuid = service.GetDistributionId(distributionName);
    service.SetDefaultDistribution(&distroGuid);
    wsl::windows::common::wslutil::PrintSystemError(ERROR_SUCCESS);
    return 0;
}

int SetDefaultVersion(_In_ std::wstring_view commandLine)
{
    const auto argument = wsl::windows::common::helpers::ParseArgument(commandLine);
    const auto version = ParseVersionString(argument);
    if (version == LXSS_WSL_VERSION_1)
    {
        THROW_HR_IF(WSL_E_WSL1_NOT_SUPPORTED, !wsl::windows::common::helpers::IsWslOptionalComponentPresent());
    }
    else
    {
        WI_ASSERT(version == LXSS_WSL_VERSION_2);

        wsl::windows::common::wslutil::PrintMessage(wsl::shared::Localization::MessageVmModeConversionInfo());
    }

    const wil::unique_hkey lxssKey = wsl::windows::common::registry::OpenLxssUserKey();
    wsl::windows::common::registry::WriteDword(lxssKey.get(), nullptr, LXSS_WSL_DEFAULT_VERSION, version);
    wsl::windows::common::wslutil::PrintSystemError(ERROR_SUCCESS);
    return 0;
}

int Shutdown(_In_ std::wstring_view commandLine)
{
    bool force = false;
    ArgumentParser parser(std::wstring{commandLine}, WSL_BINARY_NAME);
    parser.AddArgument(force, WSL_SHUTDOWN_OPTION_FORCE);

    parser.Parse();

    wsl::windows::common::SvcComm service;
    service.Shutdown(force);

    return 0;
}

int SetSparse(GUID& distroGuid, bool sparse, bool allowUnsafe)
{
    wsl::windows::common::SvcComm service;

    auto setProgress = wsl::windows::common::ConsoleProgressIndicator(wsl::shared::Localization::MessageConversionStart());
    THROW_IF_FAILED(service.SetSparse(&distroGuid, sparse, allowUnsafe));

    return 0;
}

int SetVersion(_In_ std::wstring_view commandLine)
{
    auto argument = wsl::windows::common::helpers::ParseArgument(commandLine);
    if (argument.empty())
    {
        wsl::windows::common::wslutil::PrintMessage(Localization::MessageRequiredParameterMissing(WSL_SET_VERSION_ARG), stdout);
        return -1;
    }

    const std::wstring distributionName(argument);
    wsl::windows::common::SvcComm service;
    const auto distroGuid = service.GetDistributionId(distributionName.c_str());

    commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
    argument = wsl::windows::common::helpers::ParseArgument(commandLine);
    const auto version = ParseVersionString(argument);
    if (version == LXSS_WSL_VERSION_2)
    {
        wsl::windows::common::wslutil::PrintMessage(wsl::shared::Localization::MessageVmModeConversionInfo());
    }

    auto progress = wsl::windows::common::ConsoleProgressIndicator(wsl::shared::Localization::MessageConversionStart(), true);
    const auto result = service.SetVersion(&distroGuid, version);
    progress.End();
    THROW_IF_FAILED(result);

    wsl::windows::common::wslutil::PrintSystemError(ERROR_SUCCESS);
    return 0;
}

int Status()
{
    // Print the default distro.
    wsl::windows::common::SvcComm service;
    const auto distros = service.EnumerateDistributions();
    for (const auto& entry : distros)
    {
        if (WI_IsFlagSet(entry.Flags, LXSS_ENUMERATE_FLAGS_DEFAULT))
        {
            wsl::windows::common::wslutil::PrintMessage(Localization::MessageStatusDefaultDistro(entry.DistroName), stdout);
            break;
        }
    }

    // Print the default version.
    const DWORD version = wsl::windows::common::wslutil::GetDefaultVersion();
    wsl::windows::common::wslutil::PrintMessage(Localization::MessageStatusDefaultVersion(version), stdout);

    // Print a message if the WSL optional component is not present for WSL1 support.
    if (!wsl::windows::common::helpers::IsWslOptionalComponentPresent())
    {
        wsl::windows::common::wslutil::PrintMessage(wsl::shared::Localization::MessageWsl1NotSupported());
    }

    // Print a message if the vmcompute service is present for WSL2 support.
    if (!wsl::windows::common::helpers::IsServicePresent(L"vmcompute"))
    {
        wsl::windows::common::wslutil::PrintMessage(wsl::shared::Localization::MessageEnableVirtualization());
    }

    return 0;
}

int TerminateDistribution(_In_ LPCWSTR distributionName)
{
    wsl::windows::common::SvcComm service;
    const GUID distroGuid = service.GetDistributionId(distributionName);
    service.TerminateInstance(&distroGuid);
    wsl::windows::common::wslutil::PrintSystemError(ERROR_SUCCESS);
    return 0;
}

int Unmount(_In_ const std::wstring& arg)
{
    const auto* disk = arg.empty() ? nullptr : arg.c_str();

    std::pair<int, int> value;
    wsl::windows::common::SvcComm service;
    const HRESULT result = wil::ResultFromException([&] { value = service.DetachDisk(disk); });

    // support relative paths in unmount
    // check is the result is the error code for "file not found" and the path is relative
    if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && PathIsRelative(disk))
    {
        // retry dismounting with the absolute path
        const auto absoluteDisk = wsl::windows::common::filesystem::GetFullPath(filesystem::UnquotePath(disk).c_str());
        value = service.DetachDisk(absoluteDisk.c_str());
    }
    else if (FAILED(result))
    {
        THROW_HR(result);
    }

    if (value.first != 0)
    {
        wsl::windows::common::wslutil::PrintMessage(Localization::MessageDetachFailed(strerror(-value.first), WSL_SHUTDOWN_ARG), stdout);
        return -1;
    }

    wsl::windows::common::wslutil::PrintSystemError(ERROR_SUCCESS);
    return 0;
}

int UnregisterDistribution(_In_ LPCWSTR distributionName)
{
    auto progress = wsl::windows::common::ConsoleProgressIndicator(wsl::shared::Localization::MessageStatusUnregistering(), true);
    wsl::windows::common::SvcComm service;
    const GUID distroGuid = service.GetDistributionId(distributionName, LXSS_GET_DISTRO_ID_LIST_ALL);
    service.UnregisterDistribution(&distroGuid);
    progress.End();
    wsl::windows::common::wslutil::PrintSystemError(ERROR_SUCCESS);
    return 0;
}

int UpdatePackage(std::wstring_view commandLine)
{
    ExecutionContext context(wsl::windows::common::UpdatePackage);

    bool preRelease{};
    ArgumentParser parser(std::wstring{commandLine}, WSL_BINARY_NAME);
    parser.AddArgument(preRelease, WSL_UPDATE_ARG_PRE_RELEASE_OPTION_LONG);

    // Options kept for compatibility with inbox WSL.
    parser.AddArgument(NoOp(), WSL_UPDATE_ARG_WEB_DOWNLOAD_OPTION_LONG);
    parser.AddArgument(NoOp(), WSL_UPDATE_ARG_CONFIRM_OPTION_LONG);
    parser.AddArgument(NoOp(), WSL_UPDATE_ARG_PROMPT_OPTION_LONG);
    parser.Parse();

    return wsl::windows::common::wslutil::UpdatePackage(preRelease, false);
}

int Uninstall()
{
    auto logFile = std::filesystem::temp_directory_path() / L"wsl-uninstall-logs.txt";
    auto clearLogs =
        wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&logFile]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFile(logFile.c_str())); });

    const auto exitCode = wsl::windows::common::wslutil::UninstallViaMsi(logFile.c_str(), &wsl::windows::common::wslutil::MsiMessageCallback);

    if (exitCode != 0)
    {
        clearLogs.release();
        THROW_HR_WITH_USER_ERROR(
            HRESULT_FROM_WIN32(exitCode),
            wsl::shared::Localization::MessageUninstallFailed(exitCode) + L"\r\n" +
                wsl::shared::Localization::MessageSeeLogFile(logFile.c_str()));
    }

    return exitCode;
}

int Version()
{
    // Query the Windows version.
    const auto windowsVersion = wsl::windows::common::helpers::GetWindowsVersionString();
    wsl::windows::common::wslutil::PrintMessage(
        Localization::MessagePackageVersions(
            WSL_PACKAGE_VERSION, KERNEL_VERSION, WSLG_VERSION, MSRDC_VERSION, DIRECT3D_VERSION, DXCORE_VERSION, windowsVersion),
        stdout);

    if constexpr (!wsl::shared::OfficialBuild)
    {
        // Print additional information if running a debug build.
        wsl::windows::common::wslutil::PrintMessage(Localization::MessageBuildInfo(_MSC_VER, COMMIT_HASH, __TIME__ " " __DATE__), stdout);
    }

    return 0;
}

int WslconfigMain(_In_ int argc, _In_reads_(argc) LPWSTR* argv)
{
    // Call the MSI package if we're in an MSIX context
    if (wsl::windows::common::wslutil::IsRunningInMsix())
    {
        return wsl::windows::common::wslutil::CallMsiPackage();
    }

    using wsl::shared::string::IsEqual;

    // Use exit code -1 on generic failures. This was the original exit code and shouldn't be changed, especially since wslconfig.exe is deprecated.
    int exitCode = -1;
    if ((argc >= 2) && ((IsEqual(argv[1], WSLCONFIG_COMMAND_LIST, true)) || (IsEqual(argv[1], WSLCONFIG_COMMAND_LIST_SHORT, true))))
    {
        ListOptions options{};
        for (int index = 2; index < argc; index += 1)
        {
            std::wstring_view argument = argv[index];
            if (argument.empty())
            {
                break;
            }
            if (IsEqual(argument, WSLCONFIG_COMMAND_LIST_ALL, true))
            {
                options.all = true;
            }
            else if (IsEqual(argument, WSLCONFIG_COMMAND_LIST_RUNNING, true))
            {
                options.running = true;
            }
            else
            {
                THROW_HR(WSL_E_INVALID_USAGE);
            }
        }

        exitCode = ListDistributionsHelper(options);
    }
    else if ((argc >= 3) && ((IsEqual(argv[1], WSLCONFIG_COMMAND_SET_DEFAULT, true)) || (IsEqual(argv[1], WSLCONFIG_COMMAND_SET_DEFAULT_SHORT, true))))
    {
        exitCode = SetDefaultDistribution(argv[2]);
    }
    else if ((argc >= 3) && ((IsEqual(argv[1], WSLCONFIG_COMMAND_TERMINATE, true)) || (IsEqual(argv[1], WSLCONFIG_COMMAND_TERMINATE_SHORT, true))))
    {
        exitCode = TerminateDistribution(argv[2]);
    }
    else if ((argc >= 3) && ((IsEqual(argv[1], WSLCONFIG_COMMAND_UNREGISTER_DISTRIBUTION, true)) || (IsEqual(argv[1], WSLCONFIG_COMMAND_UNREGISTER_DISTRIBUTION_SHORT, true))))
    {
        exitCode = UnregisterDistribution(argv[2]);
    }
    else
    {
        THROW_HR(WSL_E_INVALID_USAGE);
    }

    return exitCode;
}

int WslgMain(_In_ std::wstring_view commandLine)
{
    // N.B. There is no app execution alias for wslg, so it cannot run in an MSIX context.
    WI_ASSERT(!wsl::windows::common::wslutil::IsRunningInMsix());

    auto options = ParseLegacyArguments(commandLine);

    // Parse additional arguments.
    std::wstring_view argument;
    ShellExecOptions shellExecOptions{};
    wsl::windows::common::SvcComm service;
    for (;;)
    {
        argument = wsl::windows::common::helpers::ParseArgument(commandLine);
        if (argument.empty())
        {
            break;
        }

        if ((argument == WSL_DISTRO_ARG) || (argument == WSL_DISTRO_ARG_LONG))
        {
            THROW_HR_IF(WSL_E_INVALID_USAGE, options.DistroGuid.has_value());

            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            argument = wsl::windows::common::helpers::ParseArgument(commandLine);
            THROW_HR_IF(WSL_E_INVALID_USAGE, argument.empty());

            // Query the service for the distribution id.
            options.DistroGuid = service.GetDistributionId(std::wstring(argument).c_str());
        }
        else if (argument == WSL_SHELL_OPTION_ARG)
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            argument = wsl::windows::common::helpers::ParseArgument(commandLine);
            THROW_HR_IF(E_INVALIDARG, argument.empty());

            shellExecOptions.ParseShellOptionArg(argument);
        }
        else if ((argument == WSL_USER_ARG) || (argument == WSL_USER_ARG_LONG))
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            argument = wsl::windows::common::helpers::ParseArgument(commandLine);
            THROW_HR_IF(WSL_E_INVALID_USAGE, argument.empty());

            options.Username = argument;
        }
        else if (argument == WSL_CHANGE_DIRECTORY_ARG)
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            argument = wsl::windows::common::helpers::ParseArgument(commandLine, true);
            ChangeDirectory(argument, options);
        }
        else if (argument == WSL_STOP_PARSING_ARG)
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            break;
        }
        else
        {
            THROW_HR_IF(WSL_E_INVALID_USAGE, ((argument.size() > 0) && (argument[0] == L'-')));

            break;
        }

        commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
    }

    // Launching a graphical application requires a non-empty command line.
    THROW_HR_IF(WSL_E_INVALID_USAGE, commandLine.empty());

    std::vector<const wchar_t*> arguments;
    const std::wstring commandLineString{commandLine};
    wil::unique_hlocal_ptr<LPWSTR[]> execArguments{};
    LPCWSTR filename{};
    if (!shellExecOptions.IsUseShell())
    {
        int argc;
        execArguments.reset(CommandLineToArgvW(commandLineString.c_str(), &argc));
        THROW_HR_IF(E_INVALIDARG, (!execArguments || (argc == 0)));

        arguments.reserve(argc);
        arguments.insert(arguments.begin(), &execArguments.get()[0], &execArguments.get()[argc]);
        filename = arguments[0];
    }
    else
    {
        arguments.push_back(commandLineString.c_str());
    }

    // Graphical applications by default will use a login shell so that users can modify behavior.
    shellExecOptions.DefaultUseShell = true;
    shellExecOptions.DefaultLogin = shellExecOptions.IsUseShell();
    if (shellExecOptions.IsLogin())
    {
        // Launch via the user's default shell in login mode to parse files like /etc/profile.
        WI_SetFlag(options.LaunchFlags, LXSS_LAUNCH_FLAG_SHELL_LOGIN);
    }

    return LaunchProcess(filename, gsl::narrow_cast<int>(arguments.size()), arguments.data(), options);
}

int RunDebugShell()
{
    ExecutionContext context(Context::DebugShell);

    auto token = wil::open_current_access_token();
    auto tokenInfo = wil::get_token_information<TOKEN_USER>(token.get());
    auto pipePath = wsl::windows::common::wslutil::GetDebugShellPipeName(tokenInfo->User.Sid);
    wil::unique_hfile pipe{CreateFileW(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};

    if (!pipe)
    {
        auto error = GetLastError();
        if (error == ERROR_ACCESS_DENIED && !wsl::windows::common::security::IsTokenElevated(token.get()))
        {
            wsl::windows::common::wslutil::PrintMessage(wsl::shared::Localization::MessageAdministratorAccessRequiredForDebugShell());
            return 1;
        }
        else if (
            error == ERROR_FILE_NOT_FOUND &&
            !wsl::windows::policies::IsFeatureAllowed(wsl::windows::policies::OpenPoliciesKey().get(), wsl::windows::policies::c_allowDebugShellUserSetting))
        {
            wsl::windows::common::wslutil::PrintMessage(wsl::shared::Localization::MessageDebugShellDisabled());
            return 1;
        }
        else
        {
            THROW_WIN32(error);
        }
    }

    // agetty waits for a LF before printing the prompt, so write it immediately after the pipe is opened.
    // This is needed because without the '-w' flag, agetty doesn't wait and prints the shell prompt before
    // a pipe is connected, so it's lost.
    THROW_IF_WIN32_BOOL_FALSE(WriteFile(pipe.get(), "\n", 1, nullptr, nullptr));

    // Create a thread to realy stdin to the pipe.
    wsl::windows::common::SvcCommIo Io;
    auto exitEvent = wil::unique_event(wil::EventOptions::ManualReset);
    std::thread inputThread(
        [&]() { wsl::windows::common::RelayStandardInput(GetStdHandle(STD_INPUT_HANDLE), pipe.get(), {}, exitEvent.get(), &Io); });

    auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        exitEvent.SetEvent();
        inputThread.join();
    });

    // Relay the contents of the pipe to stdout.
    wsl::windows::common::relay::InterruptableRelay(pipe.get(), GetStdHandle(STD_OUTPUT_HANDLE));

    // Print a message that the VM has exited and signal the input thread to exit.
    fputws(L"\n", stdout);
    THROW_HR(HCS_E_CONNECTION_CLOSED);
}

int WslMain(_In_ std::wstring_view commandLine)
{
    // Call the MSI package if we're in an MSIX context
    if (wsl::windows::common::wslutil::IsRunningInMsix())
    {
        return wsl::windows::common::wslutil::CallMsiPackage();
    }

    // Use exit code -1 so invokers of wsl.exe can distinguish between a Linux
    // process failure and a wsl.exe failure. The distro launcher sample depends
    // on this specific code.
    int exitCode = -1;

    // Parse the command line to determine if the legacy distro GUID or the '~' argument were specified.
    auto options = ParseLegacyArguments(commandLine);

    // Parse additional arguments.
    std::wstring_view argument;
    ShellExecOptions shellExecOptions{};
    for (;;)
    {
        argument = wsl::windows::common::helpers::ParseArgument(commandLine);
        if (argument.empty())
        {
            break;
        }

        if (argument == WSL_DEBUG_SHELL_ARG_LONG)
        {
            return RunDebugShell();
        }
        else if ((argument == WSL_DISTRO_ARG) || (argument == WSL_DISTRO_ARG_LONG))
        {
            // Ensure the distribution has not already been set.
            if (options.DistroGuid.has_value())
            {
                wsl::windows::common::wslutil::PrintMessage(wsl::shared::Localization::MessageDistroAlreadySet());
                return exitCode;
            }

            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            argument = wsl::windows::common::helpers::ParseArgument(commandLine);
            if (argument.empty())
            {
                wsl::windows::common::wslutil::PrintMessage(Localization::MessageRequiredParameterMissing(WSL_DISTRO_ARG_LONG), stdout);
                return exitCode;
            }

            // Query the service for the distribution id.
            wsl::windows::common::SvcComm service;
            options.DistroGuid = service.GetDistributionId(std::wstring(argument).c_str());
        }
        else if (argument == WSL_CHANGE_DIRECTORY_ARG)
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            argument = wsl::windows::common::helpers::ParseArgument(commandLine, true);
            ChangeDirectory(argument, options);
        }
        else if (argument == WSL_DISTRIBUTION_ID_ARG)
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            argument = wsl::windows::common::helpers::ParseArgument(commandLine);

            if (argument.empty())
            {
                wsl::windows::common::wslutil::PrintMessage(Localization::MessageRequiredParameterMissing(WSL_DISTRIBUTION_ID_ARG), stdout);
                return exitCode;
            }

            options.DistroGuid = wsl::shared::string::ToGuid(argument);
            THROW_HR_IF(E_INVALIDARG, !options.DistroGuid.has_value());
        }
        else if ((argument == WSL_USER_ARG) || (argument == WSL_USER_ARG_LONG))
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            argument = wsl::windows::common::helpers::ParseArgument(commandLine);
            if (argument.empty())
            {
                wsl::windows::common::wslutil::PrintMessage(Localization::MessageRequiredParameterMissing(WSL_USER_ARG_LONG), stdout);
                return exitCode;
            }

            options.Username = argument;
        }
        else if (argument == WSL_UPDATE_ARG)
        {
            return UpdatePackage(commandLine);
        }
        else if (argument == WSL_HELP_ARG)
        {
            wsl::windows::common::wslutil::PrintMessage(Localization::MessageWslUsage());
            return exitCode;
        }
        else if (argument == WSL_STOP_PARSING_ARG)
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            break;
        }
        else if ((argument == WSL_EXEC_ARG) || (argument == WSL_EXEC_ARG_LONG))
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            shellExecOptions.SetExecMode();
            break;
        }
        else if (argument == WSL_SHELL_OPTION_ARG)
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            argument = wsl::windows::common::helpers::ParseArgument(commandLine);
            if (argument.empty())
            {
                wsl::windows::common::wslutil::PrintMessage(Localization::MessageRequiredParameterMissing(WSL_SHELL_OPTION_ARG), stdout);
                return exitCode;
            }

            shellExecOptions.ParseShellOptionArg(argument);
        }
        else if (argument == WSL_EXPORT_ARG)
        {
            return ExportDistribution(commandLine);
        }
        else if (argument == WSL_IMPORT_ARG)
        {
            return ImportDistribution(commandLine);
        }
        else if (argument == WSL_IMPORT_INPLACE_ARG)
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            return ImportDistributionInplace(commandLine);
        }
        else if ((argument == WSL_LIST_ARG) || (argument == WSL_LIST_ARG_LONG))
        {
            return ListDistributions(commandLine);
        }
        else if ((argument == WSL_SET_DEFAULT_DISTRO_ARG) || (argument == WSL_SET_DEFAULT_DISTRO_ARG_LEGACY) || (argument == WSL_SET_DEFAULT_DISTRO_ARG_LONG))
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            argument = wsl::windows::common::helpers::ParseArgument(commandLine);
            if (argument.empty())
            {
                wsl::windows::common::wslutil::PrintMessage(
                    Localization::MessageRequiredParameterMissing(WSL_SET_DEFAULT_DISTRO_ARG_LONG), stdout);
                return exitCode;
            }

            return SetDefaultDistribution(std::wstring(argument).c_str());
        }
        else if (argument == WSL_PARENT_CONSOLE_ARG)
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            argument = wsl::windows::common::helpers::ParseArgument(commandLine);
            if (argument.empty())
            {
                wsl::windows::common::wslutil::PrintMessage(Localization::MessageRequiredParameterMissing(WSL_PARENT_CONSOLE_ARG), stdout);
                return exitCode;
            }

            const auto parentProcessId = std::stoi(std::wstring(argument));

            FreeConsole();
            THROW_IF_WIN32_BOOL_FALSE(AttachConsole(parentProcessId));
        }
        else if ((argument == WSL_TERMINATE_ARG) || (argument == WSL_TERMINATE_ARG_LONG))
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            argument = wsl::windows::common::helpers::ParseArgument(commandLine);
            if (argument.empty())
            {
                wsl::windows::common::wslutil::PrintMessage(Localization::MessageRequiredParameterMissing(WSL_TERMINATE_ARG_LONG), stdout);
                return exitCode;
            }

            return TerminateDistribution(std::wstring(argument).c_str());
        }
        else if (argument == WSL_UNREGISTER_ARG)
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            argument = wsl::windows::common::helpers::ParseArgument(commandLine);
            if (argument.empty())
            {
                wsl::windows::common::wslutil::PrintMessage(Localization::MessageRequiredParameterMissing(WSL_UNREGISTER_ARG), stdout);
                return exitCode;
            }

            return UnregisterDistribution(std::wstring(argument).c_str());
        }
        else if (argument == WSL_SET_DEFAULT_VERSION_ARG)
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            return SetDefaultVersion(commandLine);
        }
        else if (argument == WSL_SHUTDOWN_ARG)
        {
            return Shutdown(commandLine);
        }
        else if (argument == WSL_MANAGE_ARG)
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            return Manage(commandLine);
        }
        else if (argument == WSL_SET_VERSION_ARG)
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            return SetVersion(commandLine);
        }
        else if (argument == WSL_MOUNT_ARG)
        {
            return Mount(commandLine);
        }
        else if (argument == WSL_UNMOUNT_ARG)
        {
            commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
            return Unmount(std::wstring(commandLine));
        }
        else if (argument == WSL_INSTALL_ARG)
        {
            return Install(commandLine);
        }
        else if (argument == WSL_SYSTEM_DISTRO_ARG)
        {
            WI_SetFlag(options.LaunchFlags, LXSS_LAUNCH_FLAG_USE_SYSTEM_DISTRO);
        }
        else if (argument == WSL_STATUS_ARG)
        {
            return Status();
        }
        else if ((argument == WSL_VERSION_ARG) || (argument == WSL_VERSION_ARG_LONG))
        {
            return Version();
        }
        else if (argument == WSL_UNINSTALL_ARG)
        {
            return Uninstall();
        }
        else
        {
            if ((argument.size() > 0) && (argument[0] == L'-'))
            {
                std::wstring InvalidArgument(argument);
                wsl::windows::common::wslutil::PrintMessage(Localization::MessageInvalidCommandLine(InvalidArgument, WSL_BINARY_NAME), stdout);
                return exitCode;
            }

            break;
        }

        commandLine = wsl::windows::common::helpers::ConsumeArgument(commandLine, argument);
    }

    // There are three possible cases:
    //     1. Empty command line - Launch the default user's default shell.
    //     2. Exec mode - Call CommandLineToArgvW on the remaining command
    //        line and pass it along to the create process call.
    //     3. Non-empty command line - The command is invoked through the
    //        default user's default shell via '$SHELL -c commandLine'.
    int argc = 0;
    LPCWSTR* arguments{};
    LPCWSTR argv[1];
    std::wstring commandLineString{commandLine};
    wil::unique_hlocal_ptr<LPWSTR[]> execArguments{};
    LPCWSTR filename{};
    if (!commandLine.empty())
    {
        if (!shellExecOptions.IsUseShell())
        {
            execArguments.reset(CommandLineToArgvW(commandLineString.c_str(), &argc));
            THROW_HR_IF(E_INVALIDARG, (!execArguments || (argc == 0)));

            arguments = const_cast<LPCWSTR*>(execArguments.get());
            filename = arguments[0];
        }
        else
        {
            argv[0] = commandLineString.c_str();
            arguments = argv;
            argc = RTL_NUMBER_OF(argv);
        }
    }
    else
    {
        THROW_HR_IF(E_INVALIDARG, !shellExecOptions.IsUseShell());
    }

    shellExecOptions.DefaultLogin = shellExecOptions.IsUseShell() && commandLine.empty();
    WI_SetFlagIf(options.LaunchFlags, LXSS_LAUNCH_FLAG_SHELL_LOGIN, shellExecOptions.IsLogin());

    // Launch the process.
    return LaunchProcess(filename, argc, arguments, options);
}

} // namespace

int wsl::windows::common::WslClient::Main(_In_ LPCWSTR commandLine)
{
    wsl::windows::common::EnableContextualizedErrors(false);

    // Note WslTraceLoggingUninitialize() is a no-op if WslTraceLoggingInitialize was not called.
    auto cleanupTelemetry = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { WslTraceLoggingUninitialize(); });

    std::optional<wsl::windows::common::ExecutionContext> context;
    auto entryPoint = Entrypoint::Wsl;
    DWORD exitCode;
    HRESULT result = S_OK;
    try
    {
        wsl::windows::common::wslutil::ConfigureCrt();
        wsl::windows::common::wslutil::InitializeWil();
        WslTraceLoggingInitialize(LxssTelemetryProvider, !wsl::shared::OfficialBuild);

        // Set CRT encoding.
        const char* encoding = getenv("WSL_UTF8");
        if (encoding != nullptr && strcmp(encoding, "1") == 0)
        {
            wsl::windows::common::wslutil::SetCrtEncoding(_O_U8TEXT);
        }
        else
        {
            wsl::windows::common::wslutil::SetCrtEncoding(_O_U16TEXT);
        }

        // Initialize COM.
        auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
        wsl::windows::common::wslutil::CoInitializeSecurity();

        auto cleanupWinrt = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { winrt::clear_factory_cache(); });

        // Initialize winsock.
        WSADATA data;
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));

        // Determine which entrypoint to use.
        int argc = 0;
        wil::unique_hlocal_ptr<LPWSTR[]> argv{CommandLineToArgvW(commandLine, &argc)};
        THROW_HR_IF(E_INVALIDARG, (!argv || (argc == 0)));

        auto fileName = std::filesystem::path(argv[0]).stem().wstring();
        std::transform(fileName.begin(), fileName.end(), fileName.begin(), tolower);

        FILE* warningsFile = nullptr;
        const char* disableWarnings = getenv("WSL_DISABLE_WARNINGS");
        if (disableWarnings == nullptr || strcmp(disableWarnings, "1") != 0)
        {
            warningsFile = stderr;
        }

        if (fileName == L"bash")
        {
            entryPoint = Entrypoint::Bash;
            context.emplace(Context::Bash, warningsFile);
            exitCode = BashMain(commandLine);
        }
        else if (fileName == L"wslconfig")
        {
            entryPoint = Entrypoint::Wslconfig;
            context.emplace(Context::WslConfig, warningsFile);
            exitCode = WslconfigMain(argc, argv.get());
        }
        else if (fileName == L"wslg")
        {
            entryPoint = Entrypoint::Wslg;
            context.emplace(Context::Wslg, warningsFile);
            exitCode = WslgMain(commandLine);
        }
        else
        {
            context.emplace(Context::Wsl, warningsFile);
            exitCode = WslMain(commandLine);
        }
    }
    catch (...)
    {
        // N.B. bash.exe historically has used 1 instead of -1 to indicate failure.
        exitCode = (entryPoint == Entrypoint::Bash) ? 1 : -1;
        result = wil::ResultFromCaughtException();
    }

    // Print error messages for failures.
    if (FAILED(result))
        try
        {
            std::wstring errorString{};
            if (context.has_value() && context->ReportedError().has_value())
            {
                auto strings = wsl::windows::common::wslutil::ErrorToString(context->ReportedError().value());

                // Don't print the error code for WSL_E_DEFAULT_DISTRO_NOT_FOUND and WSL_E_INVALID_USAGE to make the error message easier to read.
                if (context->ReportedError()->Code != WSL_E_DEFAULT_DISTRO_NOT_FOUND && context->ReportedError()->Code != WSL_E_INVALID_USAGE)
                {
                    errorString = Localization::MessageErrorCode(strings.Message, strings.Code);
                }
                else
                {
                    errorString = strings.Message.c_str();
                }

                // Logs when an error is shown to the user, and what that error is
                WSL_LOG_TELEMETRY(
                    "UserVisibleError",
                    PDT_ProductAndServicePerformance,
                    TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
                    TraceLoggingValue(strings.Code.c_str(), "ErrorCode"));
            }
            else
            {
                errorString = wsl::windows::common::wslutil::GetErrorString(result);
            }

            // For wslg.exe, attempt to print the error message to the parent console, if that fails display a messagebox.
            if ((entryPoint == Entrypoint::Wslg) && (!wsl::windows::common::helpers::TryAttachConsole()))
            {
                auto caption = wsl::shared::Localization::AppName();
                LOG_LAST_ERROR_IF(MessageBoxW(nullptr, errorString.c_str(), caption.c_str(), (MB_OK | MB_ICONEXCLAMATION)) == 0);

                g_promptBeforeExit = false;
            }
            else
            {
                wsl::windows::common::wslutil::PrintMessage(errorString);

                //
                // If the app was launched via the start menu tile, prompt for input so the
                // message does not disappear.
                // TODO: This should be replaced with launching the WSL Settings app when that is created.
                //

                if (entryPoint == Entrypoint::Wsl && winrt::Windows::ApplicationModel::AppInstance::GetActivatedEventArgs() != nullptr)
                {
                    g_promptBeforeExit = true;
                }
            }
        }
    CATCH_LOG()

    if (g_promptBeforeExit)
    {
        g_promptBeforeExit = false;
        PromptForKeyPress();
    }

    return exitCode;
}
