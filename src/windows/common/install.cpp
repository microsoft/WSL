/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    install.cpp

Abstract:

    This file contains MSI/Wintrust install helper functions.
    Split from wslutil.cpp to avoid pulling msi.dll/wintrust.dll
    into targets that don't need them.

--*/

#include "precomp.h"
#include "install.h"
#include "wslutil.h"
#include "WslPluginApi.h"
#include "wslinstallerservice.h"

#include "ConsoleProgressBar.h"
#include "ExecutionContext.h"
#include "MsiQuery.h"

using winrt::Windows::Foundation::Uri;
using winrt::Windows::Management::Deployment::DeploymentOptions;
using wsl::shared::Localization;
using wsl::windows::common::Context;
using namespace wsl::windows::common::registry;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::common::install;

namespace {

bool PromptForKeyPress()
{
    THROW_IF_WIN32_BOOL_FALSE(FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE)));

    // Note: Ctrl-c causes _getch to return 0x3.
    return _getch() != 0x3;
}

bool PromptForKeyPressWithTimeout()
{
    // Run PromptForKeyPress on a separate thread so we can apply a timeout.
    // If PromptForKeyPress fails, fulfill the promise with false so the caller doesn't hang.
    std::promise<bool> pressedKey;
    auto thread = std::thread([&pressedKey]() {
        try
        {
            pressedKey.set_value(PromptForKeyPress());
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            try
            {
                pressedKey.set_value(false);
            }
            CATCH_LOG()
        }
    });

    auto cancelRead = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&thread]() {
        if (thread.joinable())
        {
            LOG_IF_WIN32_BOOL_FALSE(CancelSynchronousIo(thread.native_handle()));
            thread.join();
        }
    });

    auto future = pressedKey.get_future();
    const auto waitResult = future.wait_for(std::chrono::minutes(1));

    return waitResult == std::future_status::ready && future.get();
}

int UpdatePackageImpl(bool preRelease, bool repair)
{
    if (!repair)
    {
        PrintMessage(Localization::MessageCheckingForUpdates());
    }

    auto [version, release] = GetLatestGitHubRelease(preRelease);

    if (!repair && ParseWslPackageVersion(version) <= wsl::shared::PackageVersion)
    {
        PrintMessage(Localization::MessageUpdateNotNeeded());
        return 0;
    }

    PrintMessage(Localization::MessageUpdatingToVersion(version.c_str()));

    const bool msiInstall = wsl::shared::string::EndsWith<wchar_t>(release.name, L".msi");
    const auto downloadPath = DownloadFile(release.url, release.name);
    if (msiInstall)
    {
        auto logFile = std::filesystem::temp_directory_path() / L"wsl-install-logs.txt";
        auto clearLogs =
            wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&logFile]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFile(logFile.c_str())); });

        const auto exitCode = UpgradeViaMsi(downloadPath.c_str(), L"", logFile.c_str(), &MsiMessageCallback);

        if (exitCode == ERROR_SUCCESS_REBOOT_REQUIRED)
        {
            PrintSystemError(ERROR_SUCCESS_REBOOT_REQUIRED);
        }
        else if (exitCode != 0)
        {
            clearLogs.release();
            THROW_HR_WITH_USER_ERROR(
                HRESULT_FROM_WIN32(exitCode),
                wsl::shared::Localization::MessageUpdateFailed(exitCode) + L"\r\n" +
                    wsl::shared::Localization::MessageSeeLogFile(logFile.c_str()));
        }
    }
    else
    {
        // Set FILE_FLAG_DELETE_ON_CLOSE on the file to make sure it's deleted when the installation completes.
        const wil::unique_hfile package{CreateFileW(
            downloadPath.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, nullptr)};

        THROW_LAST_ERROR_IF(!package);

        const winrt::Windows::Management::Deployment::PackageManager packageManager;
        const auto result = packageManager.AddPackageAsync(
            Uri{downloadPath.c_str()}, nullptr, DeploymentOptions::ForceApplicationShutdown | DeploymentOptions::ForceTargetApplicationShutdown);

        THROW_IF_FAILED(result.get().ExtendedErrorCode());

        // Note: If the installation is successful, this process is expected to receive and Ctrl-C and exit
    }

    return 0;
}

void WaitForMsiInstall()
{
    wil::com_ptr_t<IWslInstaller> installer;

    auto retry_pred = []() {
        const auto errorCode = wil::ResultFromCaughtException();
        return errorCode == REGDB_E_CLASSNOTREG;
    };

    wsl::shared::retry::RetryWithTimeout<void>(
        [&installer]() { installer = wil::CoCreateInstance<IWslInstaller>(__uuidof(WslInstaller), CLSCTX_LOCAL_SERVER); },
        std::chrono::seconds(1),
        std::chrono::minutes(1),
        retry_pred);

    fputws(wsl::shared::Localization::MessageFinishMsiInstallation().c_str(), stderr);

    auto finishLine = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { fputws(L"\n", stderr); });

    UINT exitCode = -1;
    wil::unique_cotaskmem_string message{};
    THROW_IF_FAILED(installer->Install(&exitCode, &message));

    if (message && *message.get() != UNICODE_NULL)
    {
        finishLine.release();
        wprintf(L"\n%ls\n", message.get());
    }

    if (exitCode != 0)
    {
        THROW_HR_WITH_USER_ERROR(HRESULT_FROM_WIN32(exitCode), wsl::shared::Localization::MessageUpdateFailed(exitCode));
    }
}

wil::unique_handle CreateJob()
{
    // Create a job object that will terminate all processes in the job on
    // close but will not terminate the children of the processes in the job.
    // This is used to ensure that when forwarding from an inbox binary (I)
    // to a lifted binary (L), if I is terminated L is terminated as well but
    // any children of L (e.g. wslhost.exe) continue to run.
    wil::unique_handle job{CreateJobObject(nullptr, nullptr)};
    THROW_LAST_ERROR_IF_NULL(job.get());

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &info, sizeof(info)));

    return job;
}

int WINAPI InstallRecordHandler(void* context, UINT messageType, LPCWSTR message)
{
    try
    {
        WSL_LOG("MSIMessage", TraceLoggingValue(messageType, "type"), TraceLoggingValue(message, "message"));
        auto type = (INSTALLMESSAGE)(0xFF000000 & (UINT)messageType);

        if (type == INSTALLMESSAGE_ERROR || type == INSTALLMESSAGE_FATALEXIT || type == INSTALLMESSAGE_WARNING)
        {
            WriteInstallLog(std::format("MSI message: {}", message));
        }

        auto* callback = reinterpret_cast<const std::function<void(INSTALLMESSAGE, LPCWSTR)>*>(context);
        if (callback != nullptr)
        {
            (*callback)(type, message);
        }
    }
    CATCH_LOG();

    return IDOK;
}

void ConfigureMsiLogging(_In_opt_ LPCWSTR LogFile, _In_ const std::function<void(INSTALLMESSAGE, LPCWSTR)>& Callback)
{
    if (LogFile != nullptr)
    {
        LOG_IF_WIN32_ERROR(MsiEnableLog(INSTALLLOGMODE_VERBOSE | INSTALLLOGMODE_EXTRADEBUG | INSTALLLOGMODE_PROGRESS, LogFile, 0));
    }

    MsiSetExternalUI(
        &InstallRecordHandler,
        INSTALLLOGMODE_FATALEXIT | INSTALLLOGMODE_ERROR | INSTALLLOGMODE_WARNING | INSTALLLOGMODE_USER | INSTALLLOGMODE_INFO |
            INSTALLLOGMODE_RESOLVESOURCE | INSTALLLOGMODE_OUTOFDISKSPACE | INSTALLLOGMODE_ACTIONSTART | INSTALLLOGMODE_ACTIONDATA |
            INSTALLLOGMODE_COMMONDATA | INSTALLLOGMODE_INITIALIZE | INSTALLLOGMODE_TERMINATE | INSTALLLOGMODE_SHOWDIALOG,
        (void*)&Callback);

    MsiSetInternalUI(INSTALLUILEVEL(INSTALLUILEVEL_NONE | INSTALLUILEVEL_UACONLY | INSTALLUILEVEL_SOURCERESONLY), nullptr);
}

} // namespace

int wsl::windows::common::install::CallMsiPackage()
{
    wsl::windows::common::ExecutionContext context(wsl::windows::common::CallMsi);

    auto msiPath = GetMsiPackagePath();
    if (!msiPath.has_value())
    {
        wsl::windows::common::ExecutionContext context(wsl::windows::common::Install);

        try
        {
            WaitForMsiInstall();
            msiPath = GetMsiPackagePath();
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();

            // GetMsiPackagePath() will generate a user error if the registry access fails.
            // Save the error from GetMsiPackagePath() to return a proper 'install failed' message.
            auto savedError = context.ReportedError();

            // There is a race where the service might stop before returning the install result.
            // if this happens, only fail if the MSI still isn't installed.
            msiPath = GetMsiPackagePath();
            if (!msiPath.has_value())
            {
                // Offer to directly install the MSI package if the MsixInstaller logic fails
                // This can trigger a UAC so only do it
                if (IsInteractiveConsole())
                {
                    auto errorCode = savedError.has_value() ? ErrorToString(savedError.value()).Code
                                                            : ErrorCodeToString(wil::ResultFromCaughtException());

                    EMIT_USER_WARNING(wsl::shared::Localization::MessageInstallationCorrupted(errorCode));

                    if (PromptForKeyPressWithTimeout())
                    {
                        return UpdatePackage(false, true);
                    }
                }

                if (savedError.has_value())
                {
                    THROW_HR_WITH_USER_ERROR(savedError->Code, savedError->Message.value_or(L""));
                }

                throw;
            }
        }

        THROW_HR_IF(E_UNEXPECTED, !msiPath.has_value());
    }

    auto target = msiPath.value() + L"\\" WSL_BINARY_NAME;

    SubProcess process(target.c_str(), GetCommandLine());
    process.SetDesktopAppPolicy(PROCESS_CREATION_DESKTOP_APP_BREAKAWAY_ENABLE_PROCESS_TREE);
    auto runningProcess = process.Start();

    // N.B. The job cannot be assigned at process creation time as the packaged process
    //      creation path will assign the new process to a per package job object.
    //      In the case of multiple processes running in a single package, assigning
    //      the new process to the per package job object will fail for the second request
    //      since both jobs already have processes which prevents a job hierarchy from
    //      being established.
    auto job = CreateJob();

    // Assign the process to the job, ignoring failures when the process has
    // terminated.
    //
    // N.B. Assigning the job after process creation without CREATE_SUSPENDED is
    //      safe to do here since only the new child process will be in the job
    //      object. None of the grandchildren processes are included since the
    //      job is created with JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK.
    if (!AssignProcessToJobObject(job.get(), runningProcess.get()))
    {
        auto lastError = GetLastError();
        if (lastError != ERROR_ACCESS_DENIED)
        {
            THROW_WIN32(lastError);
        }
    }

    return static_cast<int>(SubProcess::GetExitCode(runningProcess.get()));
}

void wsl::windows::common::install::MsiMessageCallback(INSTALLMESSAGE type, LPCWSTR message)
{
    switch (type)
    {
    case INSTALLMESSAGE_ERROR:
    case INSTALLMESSAGE_FATALEXIT:
    case INSTALLMESSAGE_WARNING:
        wprintf(L"%ls\n", message);
        break;

    default:
        break;
    }
}

int wsl::windows::common::install::UpdatePackage(bool PreRelease, bool Repair)
{
    // Register a console control handler so "^C" is not printed when the app platform terminates the process.
    THROW_IF_WIN32_BOOL_FALSE(SetConsoleCtrlHandler(
        [](DWORD ctrlType) {
            if (ctrlType == CTRL_C_EVENT)
            {
                ExitProcess(0);
            }
            return FALSE;
        },
        TRUE));

    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [] { SetConsoleCtrlHandler(nullptr, FALSE); });

    try
    {
        return UpdatePackageImpl(PreRelease, Repair);
    }
    catch (...)
    {
        // Rethrowing via WIL is required for the error context to be properly set in case a winrt exception was thrown.
        THROW_HR(wil::ResultFromCaughtException());
    }
}

UINT wsl::windows::common::install::UpgradeViaMsi(
    _In_ LPCWSTR PackageLocation, _In_opt_ LPCWSTR ExtraArgs, _In_opt_ LPCWSTR LogFile, _In_ const std::function<void(INSTALLMESSAGE, LPCWSTR)>& Callback)
{
    // Always suppress MSI-initiated reboots. With INSTALLUILEVEL_NONE, Windows Installer
    // will silently reboot the machine if files are in use and REBOOT is not suppressed.
    std::wstring args = L"REBOOT=ReallySuppress";
    if (ExtraArgs != nullptr && *ExtraArgs != L'\0')
    {
        args = std::wstring(ExtraArgs) + L" " + args;
    }

    WriteInstallLog(std::format("Upgrading via MSI package: {}. Args: {}", PackageLocation, args));

    ConfigureMsiLogging(LogFile, Callback);

    auto result = MsiInstallProduct(PackageLocation, args.c_str());
    WSL_LOG("MsiInstallResult", TraceLoggingValue(result, "result"), TraceLoggingValue(args.c_str(), "ExtraArgs"));

    WriteInstallLog(std::format("MSI upgrade result: {}", result));

    return result;
}

UINT wsl::windows::common::install::UninstallViaMsi(_In_opt_ LPCWSTR LogFile, _In_ const std::function<void(INSTALLMESSAGE, LPCWSTR)>& Callback)
{
    const auto key = OpenLxssMachineKey(KEY_READ);
    const auto productCode = ReadString(key.get(), L"Msi", L"ProductCode", nullptr);

    WriteInstallLog(std::format("Uninstalling MSI package: {}", productCode));

    ConfigureMsiLogging(LogFile, Callback);

    auto result = MsiConfigureProductEx(productCode.c_str(), 0, INSTALLSTATE_ABSENT, L"REBOOT=ReallySuppress");
    WSL_LOG("MsiUninstallResult", TraceLoggingValue(result, "result"));

    WriteInstallLog(std::format("MSI package uninstall result: {}", result));

    return result;
}

wil::unique_hfile wsl::windows::common::install::ValidateFileSignature(LPCWSTR Path)
{
    wil::unique_hfile fileHandle{CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
    THROW_LAST_ERROR_IF(!fileHandle);

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;

    WINTRUST_FILE_INFO file = {0};
    file.cbStruct = sizeof(file);
    file.hFile = fileHandle.get();
    trust.pFile = &file;

    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        trust.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(nullptr, &action, &trust);
    });

    THROW_IF_WIN32_ERROR(WinVerifyTrust(nullptr, &action, &trust));

    return fileHandle;
}

void wsl::windows::common::install::WriteInstallLog(const std::string& Content)
try
{
    static std::wstring path = wil::GetWindowsDirectoryW<std::wstring>() + L"\\temp\\wsl-install-log.txt";

    // Wait up to 10 seconds for the log file mutex
    wil::unique_handle mutex{CreateMutex(nullptr, true, L"Global\\WslInstallLog")};
    THROW_LAST_ERROR_IF(!mutex);

    THROW_LAST_ERROR_IF(WaitForSingleObject(mutex.get(), 10 * 1000) != WAIT_OBJECT_0);

    wil::unique_handle file{CreateFile(
        path.c_str(), GENERIC_ALL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, 0, nullptr)};

    THROW_LAST_ERROR_IF(!file);

    LARGE_INTEGER size{};
    THROW_IF_WIN32_BOOL_FALSE(GetFileSizeEx(file.get(), &size));

    // Append to the file if its size is below 10MB, otherwise truncate.
    if (size.QuadPart < 10 * _1MB)
    {
        THROW_LAST_ERROR_IF(SetFilePointer(file.get(), 0, nullptr, FILE_END) == INVALID_SET_FILE_POINTER);
    }
    else
    {
        THROW_IF_WIN32_BOOL_FALSE(SetEndOfFile(file.get()));
    }

    static auto processName = wil::GetModuleFileNameW<std::wstring>();
    auto logLine = std::format("{:%FT%TZ} {}[{}]: {}\n", std::chrono::system_clock::now(), processName, WSL_PACKAGE_VERSION, Content);

    DWORD bytesWritten{};
    THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), logLine.c_str(), static_cast<DWORD>(logLine.size()), &bytesWritten, nullptr));
}
CATCH_LOG();
