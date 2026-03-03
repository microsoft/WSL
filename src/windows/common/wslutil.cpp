/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslutil.cpp

Abstract:

    This file contains helper function definitions.

--*/

#include "precomp.h"
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

constexpr auto c_latestReleaseUrl = L"https://api.github.com/repos/Microsoft/WSL/releases/latest";
constexpr auto c_releaseListUrl = L"https://api.github.com/repos/Microsoft/WSL/releases";
constexpr auto c_specificReleaseListUrl = L"https://api.github.com/repos/Microsoft/WSL/releases/tags/";
constexpr auto c_userAgent = L"wsl-install"; // required to use the GitHub API
constexpr auto c_pipePrefix = L"\\\\.\\pipe\\";

namespace {

#define X(Error) {(Error), L## #Error}
#define X_WIN32(Error) {HRESULT_FROM_WIN32(Error), L## #Error}

static const std::map<HRESULT, LPCWSTR> g_commonErrors{
    X(WSL_E_DEFAULT_DISTRO_NOT_FOUND),
    X(WSL_E_DISTRO_NOT_FOUND),
    X(WSL_E_WSL1_NOT_SUPPORTED),
    X(WSL_E_VM_MODE_NOT_SUPPORTED),
    X(WSL_E_TOO_MANY_DISKS_ATTACHED),
    X(WSL_E_CONSOLE),
    X(WSL_E_CUSTOM_KERNEL_NOT_FOUND),
    X(WSL_E_USER_NOT_FOUND),
    X(WSL_E_INVALID_USAGE),
    X(WSL_E_EXPORT_FAILED),
    X(WSL_E_IMPORT_FAILED),
    X(WSL_E_TTY_LIMIT),
    X(WSL_E_CUSTOM_SYSTEM_DISTRO_ERROR),
    X(WSL_E_LOWER_INTEGRITY),
    X(WSL_E_HIGHER_INTEGRITY),
    X(WSL_E_FS_UPGRADE_NEEDED),
    X(WSL_E_USER_VHD_ALREADY_ATTACHED),
    X(WSL_E_VM_MODE_INVALID_STATE),
    X(WSL_E_VM_MODE_MOUNT_NAME_ALREADY_EXISTS),
    X(WSL_E_ELEVATION_NEEDED_TO_MOUNT_DISK),
    X(WSL_E_DISK_ALREADY_ATTACHED),
    X(WSL_E_DISK_ALREADY_MOUNTED),
    X(WSL_E_DISK_MOUNT_FAILED),
    X(WSL_E_DISK_UNMOUNT_FAILED),
    X(WSL_E_WSL2_NEEDED),
    X(WSL_E_VM_MODE_INVALID_MOUNT_NAME),
    X(WSL_E_GUI_APPLICATIONS_DISABLED),
    X(WSL_E_DISTRO_ONLY_AVAILABLE_FROM_STORE),
    X(WSL_E_WSL_MOUNT_NOT_SUPPORTED),
    X(WSL_E_WSL_OPTIONAL_COMPONENT_REQUIRED),
    X(WSL_E_VMSWITCH_NOT_FOUND),
    X(WSL_E_WSL_MOUNT_NOT_SUPPORTED),
    X(WSL_E_VMSWITCH_NOT_SET),
    X(WSL_E_INSTALL_PROCESS_FAILED),
    X(WSL_E_OS_NOT_SUPPORTED),
    X(WSL_E_INSTALL_COMPONENT_FAILED),
    X(WSL_E_PLUGIN_REQUIRES_UPDATE),
    X(WSL_E_DISK_MOUNT_DISABLED),
    X(WSL_E_WSL1_DISABLED),
    X(WSL_E_VIRTUAL_MACHINE_PLATFORM_REQUIRED),
    X(WSL_E_LOCAL_SYSTEM_NOT_SUPPORTED),
    X(WSL_E_DISK_CORRUPTED),
    X(WSL_E_DISTRIBUTION_NAME_NEEDED),
    X(WSL_E_INVALID_JSON),
    X(WSL_E_VM_CRASHED),
    X(WSL_E_NOT_A_LINUX_DISTRO),
    X(E_ACCESSDENIED),
    X_WIN32(ERROR_NOT_FOUND),
    X_WIN32(ERROR_VERSION_PARSE_ERROR),
    X(E_INVALIDARG),
    X_WIN32(ERROR_FILE_NOT_FOUND),
    X(WININET_E_CANNOT_CONNECT),
    X(WININET_E_NAME_NOT_RESOLVED),
    X(HTTP_E_STATUS_NOT_FOUND),
    X(HCS_E_SERVICE_NOT_AVAILABLE),
    X_WIN32(ERROR_PATH_NOT_FOUND),
    X(HCS_E_CONNECTION_TIMEOUT),
    X(E_FAIL),
    X(E_UNEXPECTED),
    X(HCN_E_ADDR_INVALID_OR_RESERVED),
    X_WIN32(RPC_S_CALL_FAILED),
    X(RPC_E_DISCONNECTED),
    X_WIN32(ERROR_PIPE_NOT_CONNECTED),
    X_WIN32(ERROR_PIPE_BUSY),
    X_WIN32(ERROR_UNSUPPORTED_TYPE),
    X_WIN32(ERROR_CANCELLED),
    X_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY),
    X_WIN32(HCS_E_HYPERV_NOT_INSTALLED),
    X(E_NOINTERFACE),
    X(REGDB_E_CLASSNOTREG),
    X(CERT_E_UNTRUSTEDROOT),
    X(E_ABORT),
    X_WIN32(ERROR_SERVICE_NOT_ACTIVE),
    X_WIN32(ERROR_SHARING_VIOLATION),
    X_WIN32(ERROR_DISK_REPAIR_DISABLED),
    X(WSL_E_DISTRO_NOT_STOPPED),
    X_WIN32(ERROR_UNHANDLED_EXCEPTION),
    X(TRUST_E_NOSIGNATURE),
    X(TRUST_E_BAD_DIGEST),
    X(E_INVALID_PROTOCOL_FORMAT),
    X_WIN32(ERROR_MOD_NOT_FOUND),
    X_WIN32(ERROR_INSTALL_USEREXIT),
    X_WIN32(ERROR_INSTALL_FAILURE),
    X_WIN32(ERROR_SERVICE_DOES_NOT_EXIST),
    X_WIN32(WSAENOTCONN),
    X_WIN32(ERROR_FILE_EXISTS),
    X_WIN32(ERROR_ALREADY_EXISTS),
    X_WIN32(ERROR_INVALID_NAME),
    X_WIN32(ERROR_NOT_SUPPORTED),
    X_WIN32(ERROR_INVALID_HANDLE),
    X_WIN32(ERROR_INVALID_DATA),
    X(HCS_E_INVALID_JSON),
    X_WIN32(ERROR_INVALID_SECURITY_DESCR),
    X(VM_E_INVALID_STATE),
    X_WIN32(STATUS_SHUTDOWN_IN_PROGRESS),
    X_WIN32(ERROR_BAD_PATHNAME),
    X(WININET_E_TIMEOUT)};

#undef X

#define X(Ctx) {Context::Ctx, L## #Ctx}

static const std::map<Context, LPCWSTR> g_contextStrings{
    X(Empty),
    X(Wsl),
    X(Wslg),
    X(Bash),
    X(WslConfig),
    X(InstallDistro),
    X(Service),
    X(RegisterDistro),
    X(CreateInstance),
    X(AttachDisk),
    X(DetachDisk),
    X(CreateVm),
    X(ParseConfig),
    X(ConfigureNetworking),
    X(ConfigureGpu),
    X(LaunchProcess),
    X(UpdatePackage),
    X(ConfigureDistro),
    X(CreateLxProcess),
    X(EnumerateDistros),
    X(ExportDistro),
    X(GetDefaultDistro),
    X(GetDistroConfiguration),
    X(GetDistroId),
    X(SetDefaultDistro),
    X(SetVersion),
    X(TerminateDistro),
    X(UnregisterDistro),
    X(RegisterLxBus),
    X(MountDisk),
    X(QueryLatestGitHubRelease),
    X(DebugShell),
    X(Plugin),
    X(CallMsi),
    X(Install),
    X(HCS),
    X(HNS),
    X(ReadDistroConfig),
    X(MoveDistro),
    X(VerifyChecksum)};

#undef X

wil::unique_hlocal_string GetWinInetErrorString(HRESULT error)
{
    const wil::unique_hmodule library{LoadLibrary(L"WinInet.dll")};
    if (!library)
    {
        return {};
    }

    wil::unique_hlocal_string message{};
    LOG_HR_IF(
        E_UNEXPECTED,
        FormatMessageW(
            (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK),
            library.get(),
            error - 0x80070000, // Mandatory to correctly resolve the error string
            0,
            wil::out_param_ptr<LPWSTR>(message),
            0,
            nullptr) == 0);

    return message;
}

bool IsWinInetError(HRESULT error)
{
    const DWORD code = error - 0x80070000;

    return code >= INTERNET_ERROR_BASE && code <= INTERNET_ERROR_LAST;
}

bool PromptForKeyPress()
{
    THROW_IF_WIN32_BOOL_FALSE(FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE)));

    // Note: Ctrl-c causes _getch to return 0x3.
    return _getch() != 0x3;
}

bool PromptForKeyPressWithTimeout()
{
    std::promise<bool> pressedKey;
    auto thread = std::thread([&pressedKey]() { pressedKey.set_value(PromptForKeyPress()); });

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

        if (exitCode != 0)
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
    wil::unique_cotaskmem_string message = nullptr;
    THROW_IF_FAILED(installer->Install(&exitCode, &message));

    if (*message.get() != UNICODE_NULL)
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

constexpr uint16_t EndianSwap(uint16_t value)
{
    return (value & 0xFF00) >> 8 | (value & 0x00FF) << 8;
}

constexpr uint32_t EndianSwap(uint32_t value)
{
    return (value & 0xFF000000) >> 24 | (value & 0x00FF0000) >> 8 | (value & 0x0000FF00) << 8 | (value & 0x000000FF) << 24;
}

constexpr unsigned long EndianSwap(unsigned long value)
{
    return gsl::narrow_cast<unsigned long>(EndianSwap(gsl::narrow_cast<uint32_t>(value)));
}

constexpr GUID EndianSwap(GUID value)
{
    value.Data1 = EndianSwap(value.Data1);
    value.Data2 = EndianSwap(value.Data2);
    value.Data3 = EndianSwap(value.Data3);
    return value;
}

} // namespace

int wsl::windows::common::wslutil::CallMsiPackage()
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

template <typename TInterface>
wil::com_ptr<TInterface> wsl::windows::common::wslutil::CoGetCallContext()
{
    wil::com_ptr<TInterface> context;
    const HRESULT hr = ::CoGetCallContext(IID_PPV_ARGS(&context));
    THROW_HR_IF(hr, FAILED(hr) && (hr != RPC_E_CALL_COMPLETE));

    return context;
}

void wsl::windows::common::wslutil::CoInitializeSecurity()
{
    THROW_IF_FAILED(CoInitializeSecurity(
        nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_STATIC_CLOAKING, 0));
}

void wsl::windows::common::wslutil::ConfigureCrt()
{
    // _CALL_REPORTFAULT will cause the process to actually crash instead of just exiting.
    _set_abort_behavior(_CALL_REPORTFAULT, _CALL_REPORTFAULT);
}

// Copied from the terminal repository:
// https://github.com/microsoft/terminal/blob/52262b05fa0a97d2d3a0fce0990840ffc0fa53f1/src/types/utils.cpp#L926
GUID wsl::windows::common::wslutil::CreateV5Uuid(const GUID& namespaceGuid, const std::span<const std::byte> name)
{
    // v5 uuid generation happens over values in network byte order, so let's enforce that
    auto correctEndianNamespaceGuid{EndianSwap(namespaceGuid)};

    wil::unique_bcrypt_hash hash;
    THROW_IF_NTSTATUS_FAILED(BCryptCreateHash(BCRYPT_SHA1_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0));

    // According to N4713 8.2.1.11 [basic.lval], accessing the bytes underlying an object
    // through unsigned char or char pointer *is defined*.
    THROW_IF_NTSTATUS_FAILED(BCryptHashData(hash.get(), reinterpret_cast<PUCHAR>(&correctEndianNamespaceGuid), sizeof(GUID), 0));
    // BCryptHashData is ill-specified in that it leaves off "const" qualification for pbInput
    THROW_IF_NTSTATUS_FAILED(
        BCryptHashData(hash.get(), reinterpret_cast<PUCHAR>(const_cast<std::byte*>(name.data())), gsl::narrow<ULONG>(name.size()), 0));

    std::array<uint8_t, 20> buffer;
    THROW_IF_NTSTATUS_FAILED(BCryptFinishHash(hash.get(), buffer.data(), gsl::narrow<ULONG>(buffer.size()), 0));

    buffer.at(6) = (buffer.at(6) & 0x0F) | 0x50; // set the uuid version to 5
    buffer.at(8) = (buffer.at(8) & 0x3F) | 0x80; // set the variant to 2 (RFC4122)

    // We're using memcpy here pursuant to N4713 6.7.2/3 [basic.types],
    // "...the underlying bytes making up the object can be copied into an array
    // of char or unsigned char...array is copied back into the object..."
    // std::copy may compile down to ::memcpy for these types, but using it might
    // contravene the standard and nobody's got time for that.
    GUID newGuid{0};
    ::memcpy_s(&newGuid, sizeof(GUID), buffer.data(), sizeof(GUID));
    return EndianSwap(newGuid);
}

std::wstring wsl::windows::common::wslutil::DownloadFile(std::wstring_view Url, std::wstring Filename)
{
    const auto lastSlash = Url.find_last_of('/');
    THROW_HR_IF(E_INVALIDARG, lastSlash == std::wstring::npos);

    if (Filename.empty())
    {
        Filename = Url.substr(lastSlash + 1);
    }

    const auto downloadFolder =
        winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(std::filesystem::temp_directory_path().wstring()).get();

    const auto file =
        downloadFolder.CreateFileAsync(Filename, winrt::Windows::Storage::CreationCollisionOption::GenerateUniqueName).get();
    auto deleteFileOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { file.DeleteAsync().get(); });

    const auto outputStream = file.OpenAsync(winrt::Windows::Storage::FileAccessMode::ReadWrite).get().GetOutputStreamAt(0);

    // By default downloaded files are cached in %appdata%/local/packages/{package-family}/AC/InetCache .
    // Disable caching since there's no reason to keep local copies of .msixbundle files.
    const winrt::Windows::Web::Http::Filters::HttpBaseProtocolFilter filter;
    filter.CacheControl().WriteBehavior(winrt::Windows::Web::Http::Filters::HttpCacheWriteBehavior::NoCache);

    const winrt::Windows::Web::Http::HttpClient client(filter);
    client.DefaultRequestHeaders().Append(L"Accept", L"application/octet-stream");
    client.DefaultRequestHeaders().Append(L"User-Agent", c_userAgent);
    const auto asyncResponse = client.GetInputStreamAsync(winrt::Windows::Foundation::Uri(Url));

    std::atomic<uint64_t> totalBytes;
    wsl::windows::common::ConsoleProgressBar progressBar;
    asyncResponse.Progress(
        [&](const winrt::Windows::Foundation::IAsyncOperationWithProgress<winrt::Windows::Storage::Streams::IInputStream, winrt::Windows::Web::Http::HttpProgress>&,
            const winrt::Windows::Web::Http::HttpProgress& progress) {
            if (progress.TotalBytesToReceive)
            {
                totalBytes = progress.TotalBytesToReceive.GetUInt64();
            }
        });

    auto download = winrt::Windows::Storage::Streams::RandomAccessStream::CopyAsync(asyncResponse.get(), outputStream);

    download.Progress([&](const auto& _, uint64_t progress) {
        if (totalBytes != 0)
        {
            progressBar.Print(progress, totalBytes);
        }
    });

    download.get();
    progressBar.Clear();
    deleteFileOnFailure.release();

    return file.Path().c_str();
}

[[nodiscard]] HANDLE wsl::windows::common::wslutil::DuplicateHandle(_In_ HANDLE Handle, _In_ std::optional<DWORD> DesiredAccess, _In_ BOOL InheritHandle)
{
    HANDLE newHandle;
    THROW_IF_WIN32_BOOL_FALSE(::DuplicateHandle(
        GetCurrentProcess(), Handle, GetCurrentProcess(), &newHandle, DesiredAccess.value_or(0), InheritHandle, DesiredAccess.has_value() ? 0 : DUPLICATE_SAME_ACCESS));

    return newHandle;
}

[[nodiscard]] HANDLE wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(_In_ HANDLE Handle)
{
    const wil::unique_handle caller = OpenCallingProcess(PROCESS_DUP_HANDLE);
    THROW_LAST_ERROR_IF(!caller);

    HANDLE newHandle;
    THROW_IF_WIN32_BOOL_FALSE(::DuplicateHandle(caller.get(), Handle, GetCurrentProcess(), &newHandle, 0, FALSE, DUPLICATE_SAME_ACCESS));

    return newHandle;
}

[[nodiscard]] HANDLE wsl::windows::common::wslutil::DuplicateHandleToCallingProcess(_In_ HANDLE Handle, _In_ std::optional<DWORD> DesiredAccess)
{
    const wil::unique_handle caller = OpenCallingProcess(PROCESS_DUP_HANDLE);
    THROW_LAST_ERROR_IF(!caller);

    HANDLE newHandle;
    THROW_IF_WIN32_BOOL_FALSE(::DuplicateHandle(
        GetCurrentProcess(), Handle, caller.get(), &newHandle, DesiredAccess.value_or(0), FALSE, DesiredAccess.has_value() ? 0 : DUPLICATE_SAME_ACCESS));

    return newHandle;
}

void wsl::windows::common::wslutil::EnforceFileLimit(LPCWSTR Path, size_t Limit, const std::function<bool(const std::filesystem::directory_entry&)>& pred)
{
    if (Limit <= 0)
    {
        return;
    }

    std::map<std::filesystem::file_time_type, std::filesystem::path> files;
    for (auto const& e : std::filesystem::directory_iterator{Path})
    {
        if (pred(e))
        {
            files.emplace(e.last_write_time(), e.path());
        }
    }

    if (files.size() < Limit)
    {
        return;
    }

    auto fileToRemove = files.begin()->second.c_str();

    WSL_LOG(
        "File limit exceeded, deleting oldest file", TraceLoggingValue(Path, "Folder"), TraceLoggingValue(fileToRemove, "File"));

    LOG_IF_WIN32_BOOL_FALSE(DeleteFile(fileToRemove));
}

std::wstring wsl::windows::common::wslutil::ErrorCodeToString(HRESULT Error)
{
    std::wstringstream output;
    const auto resultString = g_commonErrors.find(Error);
    if (resultString != g_commonErrors.end())
    {
        output << resultString->second;
    }
    else
    {
        output << L"0x" << std::hex << Error;
    }

    return output.str();
}

wsl::windows::common::ErrorStrings wsl::windows::common::wslutil::ErrorToString(const Error& error)
{
    ErrorStrings errorStrings;

    if (error.Message.has_value())
    {
        errorStrings.Message = error.Message.value();
    }
    else
    {
        errorStrings.Message = GetErrorString(error.Code);
    }

    std::wstringstream errorCode;
    bool first = true;
    const std::bitset<64> bits(error.Context);
    for (auto i = 0; i < bits.size(); i++)
    {
        if (bits[i])
        {
            auto context = static_cast<Context>(1ull << i);
            auto it = g_contextStrings.find(context);
            if (first)
            {
                first = false;
            }
            else
            {
                errorCode << L"/";
            }

            if (it == g_contextStrings.end())
            {
                errorCode << L"?(" << context << L")";
            }
            else
            {
                errorCode << it->second;
            }
        }
    }

    errorCode << "/" << ErrorCodeToString(error.Code);

    errorStrings.Code = errorCode.str();

    return errorStrings;
}

std::wstring wsl::windows::common::wslutil::ConstructPipePath(std::wstring_view PipeName)
{
    return c_pipePrefix + std::wstring(PipeName);
}

std::filesystem::path wsl::windows::common::wslutil::GetBasePath()
{
    auto path = wil::GetModuleFileNameW<std::wstring>(wil::GetModuleInstanceHandle());
    THROW_IF_FAILED(::PathCchRemoveFileSpec(path.data(), path.size()));

    path.resize(std::wcslen(path.c_str()));
    return std::filesystem::path(std::move(path));
}

std::wstring wsl::windows::common::wslutil::GetDebugShellPipeName(_In_ PSID Sid)
{
    return ConstructPipePath(std::wstring(L"wsl_debugshell_") + SidToString(Sid).get());
}

DWORD
wsl::windows::common::wslutil::GetDefaultVersion(void)
{
    DWORD version = LXSS_WSL_VERSION_2;
    const auto hr = [&version] {
        wil::unique_hkey userKey{};
        RETURN_IF_WIN32_ERROR(RegOpenCurrentUser(KEY_READ, &userKey));

        wil::unique_hkey lxssKey{};
        RETURN_IF_WIN32_ERROR(RegOpenKeyEx(userKey.get(), LXSS_REGISTRY_PATH, 0, KEY_READ, &lxssKey));

        DWORD size = sizeof(version);
        RETURN_IF_WIN32_ERROR(RegGetValueW(lxssKey.get(), nullptr, LXSS_WSL_DEFAULT_VERSION, RRF_RT_REG_DWORD, nullptr, &version, &size));

        return S_OK;
    }();

    if (FAILED(hr) && (hr != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) && (hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)))
    {
        THROW_IF_FAILED(hr);
    }

    return version;
}

std::wstring wsl::windows::common::wslutil::GetErrorString(HRESULT result)
{
    ULONG buildNumber = 0;
    std::wstring kbUrl;

    switch (result)
    {
    case E_ILLEGAL_STATE_CHANGE:
        return Localization::MessageInvalidState();

    case WSL_E_USER_NOT_FOUND:
        return Localization::MessageUserNotFound();

    case WSL_E_CONSOLE:
        return Localization::MessageInvalidConsole();

    case WSL_E_LOWER_INTEGRITY:
        return Localization::MessageLowerIntegrity();

    case WSL_E_HIGHER_INTEGRITY:
        return Localization::MessageHigherIntegrity();

    case WSL_E_DEFAULT_DISTRO_NOT_FOUND:
        return Localization::MessageNoDefaultDistro();

    case HRESULT_FROM_WIN32(WSAECONNABORTED):
    case HRESULT_FROM_WIN32(ERROR_SHUTDOWN_IN_PROGRESS):
        return Localization::MessageInstanceTerminated();

    case WSL_E_DISTRO_NOT_FOUND:
        return Localization::MessageDistroNotFound();

    case HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS):
        return Localization::MessageDistroNameAlreadyExists();

    case WSL_E_DISTRIBUTION_NAME_NEEDED:
        return Localization::MessageDistributionNameNeeded();

    case HRESULT_FROM_WIN32(ERROR_FILE_EXISTS):
        return Localization::MessageDistroInstallPathAlreadyExists();

    case WSL_E_TOO_MANY_DISKS_ATTACHED:
        return Localization::MessageTooManyDisks();

    case WSL_E_USER_VHD_ALREADY_ATTACHED:
        return Localization::MessageUserVhdAlreadyAttached();

    case WSL_E_VM_MODE_NOT_SUPPORTED:
        return Localization::MessageVmModeNotSupported();

    case HCS_E_HYPERV_NOT_INSTALLED:
        return Localization::MessageEnableVirtualization();

    case WSL_E_VM_MODE_INVALID_STATE:
        return Localization::MessageAlreadyRequestedVersion();

    case WSL_E_WSL2_NEEDED:
        return Localization::MessageWsl2Needed();

    case WSL_E_WSL1_NOT_SUPPORTED:
        return Localization::MessageWsl1NotSupported();

    case WSL_E_DISTRO_ONLY_AVAILABLE_FROM_STORE:
        return Localization::MessageDistroOnlyAvailableFromStore();

    case WSL_E_WSL_MOUNT_NOT_SUPPORTED:
        return Localization::MessageWslMountNotSupportedOnArm();

    case WSL_E_WSL_OPTIONAL_COMPONENT_REQUIRED:
        return Localization::MessageWslOptionalComponentRequired();

    case WSL_E_EXPORT_FAILED:
        return Localization::MessageExportFailed();

    case WSL_E_IMPORT_FAILED:
        return Localization::MessageImportFailed();

    case WSL_E_DISTRO_NOT_STOPPED:
        return Localization::MessageVhdInUse();

    case WSL_E_OS_NOT_SUPPORTED:
        buildNumber = helpers::GetWindowsVersion().BuildNumber;
        if (buildNumber >= helpers::WindowsBuildNumbers::Cobalt)
        {
            kbUrl = L"https://aka.ms/store-wsl-kb-win11";
        }
        else if (buildNumber >= helpers::WindowsBuildNumbers::Iron)
        {
            kbUrl = L"https://aka.ms/store-wsl-kb-winserver2022";
        }
        else if (buildNumber >= helpers::WindowsBuildNumbers::Vibranium)
        {
            kbUrl = L"https://aka.ms/store-wsl-kb-win10";
        }
        else
        {
            // Don't throw from here, the caller might be in a catch block.
            kbUrl = std::format(L"[Unexpected build number: {}]", buildNumber);
        }

        return Localization::MessageOsNotSupported(helpers::GetWindowsVersionString().c_str(), kbUrl.c_str());

    // All the errors below this comment are not supposed to be reachable here (since there's meant to be emitted from the
    // service). But if we somehow hit them here, it's better show something useful to the user.
    case WSL_E_VM_MODE_MOUNT_NAME_ALREADY_EXISTS:
        return Localization::MessageDiskMountNameAlreadyExists();

    case WSL_E_VM_MODE_INVALID_MOUNT_NAME:
        return Localization::MessageDiskMountNameInvalid();

    case WSL_E_ELEVATION_NEEDED_TO_MOUNT_DISK:
        return Localization::MessageElevationNeededToMountDisk();

    case WSL_E_DISK_ALREADY_ATTACHED:
        return Localization::MessageDiskAlreadyAttached(L"");

    case WSL_E_DISK_ALREADY_MOUNTED:
        return Localization::MessageDiskAlreadyMounted();

    case WSL_E_CUSTOM_KERNEL_NOT_FOUND:
        return Localization::MessageCustomKernelNotFound(helpers::GetWslConfigPath().c_str(), L"");

    case WSL_E_CUSTOM_SYSTEM_DISTRO_ERROR:
        return Localization::MessageCustomSystemDistroError(helpers::GetWslConfigPath().c_str());

    case WSL_E_GUI_APPLICATIONS_DISABLED:
        return Localization::GuiApplicationsDisabled(helpers::GetWslConfigPath().c_str());

    case WSL_E_VMSWITCH_NOT_FOUND:
        return Localization::MessageVmSwitchNotFound(L"", L"");

    case WSL_E_VMSWITCH_NOT_SET:
        return Localization::MessageVmSwitchNotSet();

    case WSL_E_DISK_MOUNT_DISABLED:
        return Localization::MessageWSLMountDisabled();

    case WSL_E_VIRTUAL_MACHINE_PLATFORM_REQUIRED:
        return Localization::MessageVirtualMachinePlatformNotInstalled();

    case WSL_E_LOCAL_SYSTEM_NOT_SUPPORTED:
        return Localization::MessageLocalSystemNotSupported();

    case WSL_E_DISK_CORRUPTED:
        return Localization::MessageDiskCorrupted();

    case WSL_E_NOT_A_LINUX_DISTRO:
        return Localization::MessageInvalidDistributionTar();

    case WSL_E_INVALID_USAGE:
    {
        const auto* context = wsl::windows::common::ExecutionContext::Current();
        if (context == nullptr)
        {
            // Should be unreachable, but better fallback on something.
            break;
        }

        if (WI_IsFlagSet(context->CurrentContext(), Context::Wsl))
        {
            return wsl::shared::Localization::MessageWslUsage();
        }
        else if (WI_IsFlagSet(context->CurrentContext(), Context::Wslg))
        {
            return wsl::shared::Localization::MessageWslgUsage();
        }
        else if (WI_IsFlagSet(context->CurrentContext(), Context::WslConfig))
        {
            return wsl::shared::Localization::MessageWslconfigUsage();
        }
    }
    }

    return GetSystemErrorString(result);
}

std::optional<std::pair<std::wstring, GitHubReleaseAsset>> wsl::windows::common::wslutil::GetGitHubAssetFromRelease(const GitHubRelease& Release)
{
    auto findAsset = [&Release](LPCWSTR Suffix) {
        for (const auto& asset : Release.assets)
        {
            std::wstring filename(asset.name.size(), '\0');
            std::transform(asset.name.begin(), asset.name.end(), filename.begin(), towlower);

            if (wsl::shared::string::EndsWith<wchar_t>(filename, Suffix))
            {
                return std::make_optional(std::make_pair(Release.name, asset));
            }
        }

        return std::optional<std::pair<std::wstring, GitHubReleaseAsset>>();
    };

    // Look for an MSI package first
    auto asset = findAsset(wsl::shared::Arm64 ? L".arm64.msi" : L".x64.msi");
    if (asset.has_value())
    {
        return asset.value();
    }

    // If none was found, look for an msixbundle
    asset = findAsset(L".msixbundle");

    return asset.value();
}

std::pair<std::wstring, GitHubReleaseAsset> wsl::windows::common::wslutil::GetLatestGitHubRelease(bool preRelease)
{
    ExecutionContext context(Context::QueryLatestGitHubRelease);

    auto registryKey = registry::OpenLxssMachineKey();
    const auto url =
        registry::ReadString(registryKey.get(), nullptr, c_githubUrlOverrideRegistryValue, preRelease ? c_releaseListUrl : c_latestReleaseUrl);
    WSL_LOG("PollLatestGitHubRelease", TraceLoggingValue(url.c_str(), "url"));

    winrt::Windows::Web::Http::HttpClient client;
    client.DefaultRequestHeaders().Append(L"User-Agent", c_userAgent);
    const auto response = client.GetAsync(winrt::Windows::Foundation::Uri(url)).get();
    response.EnsureSuccessStatusCode();

    return GetLatestGitHubRelease(preRelease, response.Content().ReadAsStringAsync().get().c_str());
}

std::pair<std::wstring, GitHubReleaseAsset> wsl::windows::common::wslutil::GetLatestGitHubRelease(bool preRelease, LPCWSTR releases)
{
    std::optional<GitHubRelease> parsed{};

    if (preRelease)
    {
        std::optional<std::tuple<uint32_t, uint32_t, uint32_t>> highestVersion;
        for (const auto& e : wsl::shared::FromJson<std::vector<GitHubRelease>>(releases))
        {
            auto version = ParseWslPackageVersion(e.name);
            if (!highestVersion.has_value() || version > highestVersion)
            {
                parsed.emplace(std::move(e));
                highestVersion = version;
            }
        }
    }
    else
    {
        parsed = wsl::shared::FromJson<GitHubRelease>(releases);
    }

    THROW_HR_IF(E_UNEXPECTED, !parsed.has_value());

    // Find the latest release with an msix package asset
    auto asset = GetGitHubAssetFromRelease(parsed.value());
    THROW_HR_IF_MSG(E_UNEXPECTED, !asset.has_value(), "No suitable WSL release found on github");

    return asset.value();
}

GitHubRelease wsl::windows::common::wslutil::GetGitHubReleaseByTag(_In_ const std::wstring& inTag)
{
    ExecutionContext context(Context::QueryLatestGitHubRelease);

    const winrt::Windows::Web::Http::HttpClient client;
    client.DefaultRequestHeaders().Append(L"User-Agent", c_userAgent);
    const auto url = c_specificReleaseListUrl + inTag;
    const auto response = client.GetAsync(winrt::Windows::Foundation::Uri(url)).get();
    response.EnsureSuccessStatusCode();

    const auto content = response.Content().ReadAsStringAsync().get();

    return wsl::shared::FromJson<GitHubRelease>(content.c_str());
}

int wsl::windows::common::wslutil::GetLogicalProcessorCount()
{
    std::vector<gsl::byte> buffer;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX information = nullptr;
    DWORD length = 0;
    while (!GetLogicalProcessorInformationEx(RelationProcessorCore, information, &length))
    {
        const DWORD error = GetLastError();
        if (error == ERROR_INSUFFICIENT_BUFFER)
        {
            WI_ASSERT(buffer.size() < length);
            buffer.resize(length);
            information = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
        }
        else
        {
            THROW_WIN32_MSG(error, "GetLogicalProcessorInformationEx");
        }
    }

    int processorCount = 0;
    for (size_t offset = 0; offset < buffer.size(); offset += information->Size)
    {
        information = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(&buffer[offset]);
        for (WORD group = 0; group < information->Processor.GroupCount; group += 1)
        {
            processorCount += std::popcount(information->Processor.GroupMask[group].Mask);
        }
    }

    return processorCount;
}

std::optional<std::wstring> wsl::windows::common::wslutil::GetMsiPackagePath()
{
    const auto key = OpenLxssMachineKey(KEY_READ);

    try
    {
        return ReadString(key.get(), L"Msi", L"InstallLocation", nullptr);
    }
    catch (...)
    {
        return {};
    }
}

std::wstring wsl::windows::common::wslutil::GetPackageFamilyName(_In_ HANDLE process)
{
    std::wstring packageFamilyName;
    UINT32 length = 0;
    switch (::GetPackageFamilyName(process, &length, nullptr))
    {
    case APPMODEL_ERROR_NO_PACKAGE:
        break;

    case ERROR_INSUFFICIENT_BUFFER:
        packageFamilyName.resize(length);
        THROW_IF_WIN32_ERROR(::GetPackageFamilyName(process, &length, packageFamilyName.data()));

        break;

    default:
        THROW_LAST_ERROR_MSG("GetPackageFamilyName");
    }

    return packageFamilyName;
}

std::wstring wsl::windows::common::wslutil::GetSystemErrorString(_In_ HRESULT result)
{
    wil::unique_hlocal_string message{};

    // Special treatment for wininet errors
    if (IsWinInetError(result))
    {
        message = GetWinInetErrorString(result);
    }

    if (!message)
    {
        LOG_HR_IF(
            E_UNEXPECTED,
            FormatMessageW(
                (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK),
                nullptr,
                result,
                0,
                wil::out_param_ptr<LPWSTR>(message),
                0,
                nullptr) == 0);
    }

    if (message)
    {
        return std::wstring{message.get()};
    }

    std::wstringstream stream;
    stream << std::hex << result;
    return std::wstring(L"Error: 0x" + stream.str());
}

std::vector<BYTE> wsl::windows::common::wslutil::HashFile(HANDLE file, DWORD Algorithm)
{
    wil::unique_hcryptprov provider;
    THROW_IF_WIN32_BOOL_FALSE(CryptAcquireContext(&provider, nullptr, MS_ENH_RSA_AES_PROV, PROV_RSA_AES, CRYPT_VERIFYCONTEXT | CRYPT_SILENT));

    wil::unique_hcrypthash hash;
    THROW_IF_WIN32_BOOL_FALSE(CryptCreateHash(provider.get(), Algorithm, 0, 0, &hash));

    constexpr auto bufferSize = 10 * 1024 * 1024; // 10 MB.
    std::vector<char> buffer(bufferSize);

    DWORD readBytes{};
    while (true)
    {
        THROW_IF_WIN32_BOOL_FALSE(ReadFile(file, buffer.data(), bufferSize, &readBytes, nullptr));
        if (readBytes == 0)
        {
            break;
        }

        THROW_IF_WIN32_BOOL_FALSE(CryptHashData(hash.get(), reinterpret_cast<const BYTE*>(buffer.data()), readBytes, 0));
    }

    std::vector<BYTE> fileHash(32);
    DWORD hashSize = static_cast<DWORD>(fileHash.size());

    THROW_IF_WIN32_BOOL_FALSE(CryptGetHashParam(hash.get(), HP_HASHVAL, fileHash.data(), &hashSize, 0));
    THROW_HR_IF(E_UNEXPECTED, hashSize != fileHash.size());

    return fileHash;
}

void wsl::windows::common::wslutil::InitializeWil()
{
    wil::WilInitialize_CppWinRT();

    if constexpr (!wsl::shared::Debug)
    {
        wil::g_fResultFailFastUnknownExceptions = false;
    }
}

bool wsl::windows::common::wslutil::IsConsoleHandle(HANDLE Handle)
{
    DWORD Mode;
    return GetFileType(Handle) == FILE_TYPE_CHAR && GetConsoleMode(Handle, &Mode);
}

bool wsl::windows::common::wslutil::IsInteractiveConsole()
{
    return IsConsoleHandle(GetStdHandle(STD_INPUT_HANDLE));
}

bool wsl::windows::common::wslutil::IsRunningInMsix()
{
    UINT32 dummy{};
    const auto result = GetCurrentPackageId(&dummy, nullptr);

    if (result == NOERROR || result == ERROR_INSUFFICIENT_BUFFER)
    {
        return true;
    }
    else
    {
        // It's safer to return false by default since returning true incorrectly could create an infinity of wsl.exe.
        LOG_HR_IF_MSG(E_UNEXPECTED, result != APPMODEL_ERROR_NO_PACKAGE, "Unexpected error from: %ld", result);
        return false;
    }
}

bool wsl::windows::common::wslutil::IsVhdFile(_In_ const std::filesystem::path& path)
{
    return wsl::windows::common::string::IsPathComponentEqual(path.extension().native(), c_vhdFileExtension) ||
           wsl::windows::common::string::IsPathComponentEqual(path.extension().native(), c_vhdxFileExtension);
}

std::vector<DWORD> wsl::windows::common::wslutil::ListRunningProcesses()
{
    std::vector<DWORD> pids(1024);
    DWORD bytesReturned = 0;
    while (!EnumProcesses(pids.data(), (DWORD)pids.size() * sizeof(DWORD), &bytesReturned))
    {
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_MORE_DATA);

        pids.resize(pids.size() * 2);
    }

    pids.resize(bytesReturned / sizeof(DWORD));

    return pids;
}

void wsl::windows::common::wslutil::MsiMessageCallback(INSTALLMESSAGE type, LPCWSTR message)
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

std::pair<wil::unique_hfile, wil::unique_hfile> wsl::windows::common::wslutil::OpenAnonymousPipe(DWORD Size, bool ReadPipeOverlapped, bool WritePipeOverlapped)
{
    // Default to 4096 byte buffer, just like CreatePipe().
    if (Size == 0)
    {
        Size = 4096;
    }

    // Open the pipe device. Performing a relative open against this will
    // create an anonymous pipe.
    const wil::unique_hfile pipeDevice{
        CreateFileW(L"\\\\.\\pipe\\", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};

    THROW_LAST_ERROR_IF(!pipeDevice);

    LARGE_INTEGER timeout{};
    timeout.QuadPart = -10 * 1000 * 1000 * 120; // 120 seconds (doesn't actually matter)

    UNICODE_STRING empty{};
    OBJECT_ATTRIBUTES objectAttributes;
    InitializeObjectAttributes(&objectAttributes, &empty, 0, pipeDevice.get(), nullptr);

    IO_STATUS_BLOCK ioStatusBlock{};

    wil::unique_hfile readPipe;
    THROW_IF_NTSTATUS_FAILED(NtCreateNamedPipeFile(
        &readPipe,
        GENERIC_READ | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
        &objectAttributes,
        &ioStatusBlock,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_CREATE,
        ReadPipeOverlapped ? 0 : FILE_SYNCHRONOUS_IO_NONALERT,
        0,
        0,
        0,
        1,
        Size,
        Size,
        &timeout));

    InitializeObjectAttributes(&objectAttributes, &empty, 0, readPipe.get(), nullptr);

    wil::unique_hfile writePipe;
    THROW_IF_NTSTATUS_FAILED(NtOpenFile(
        &writePipe,
        GENERIC_WRITE | SYNCHRONIZE | FILE_READ_ATTRIBUTES,
        &objectAttributes,
        &ioStatusBlock,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        (WritePipeOverlapped ? 0 : FILE_SYNCHRONOUS_IO_NONALERT) | FILE_NON_DIRECTORY_FILE));

    return {std::move(readPipe), std::move(writePipe)};
}

bool wsl::windows::common::wslutil::IsVirtualMachinePlatformInstalled()
{
    // Note for Windows 11 22H2 and above builds: If hyper-v is installed but VMP platform isn't, HNS and vmcompute are available
    // but calls to HNS will fail if vfpext isn't installed.
    return wsl::windows::common::helpers::IsServicePresent(L"HNS") &&
           wsl::windows::common::helpers::IsServicePresent(L"vmcompute") &&
           (helpers::GetWindowsVersion().BuildNumber < helpers::WindowsBuildNumbers::Nickel ||
            wsl::windows::common::helpers::IsServicePresent(L"vfpext"));
}

wil::unique_handle wsl::windows::common::wslutil::OpenCallingProcess(_In_ DWORD access)
{
    wil::unique_handle caller{};
    const auto context = wsl::windows::common::wslutil::CoGetCallContext<ICallingProcessInfo>();
    if (context)
    {
        THROW_IF_FAILED(context->OpenCallerProcessHandle(access, &caller));
    }

    return caller;
}

std::tuple<uint32_t, uint32_t, uint32_t> wsl::windows::common::wslutil::ParseWslPackageVersion(_In_ const std::wstring& Version)
{
    const std::wregex pattern(L"(\\d+)\\.(\\d+)\\.(\\d+).*");
    std::wsmatch match;
    if (!std::regex_match(Version, match, pattern) || match.size() != 4)
    {
        THROW_HR_MSG(E_UNEXPECTED, "Failed to parse WSL package version: '%ls'", Version.c_str());
    }

    auto get = [&](int position) { return std::stoul(match.str(position)); };

    try
    {
        return std::make_tuple(get(1), get(2), get(3));
    }
    catch (const std::exception& e)
    {
        THROW_HR_MSG(E_UNEXPECTED, "Failed to parse WSL package version: '%ls', %hs", Version.c_str(), e.what());
    }
}

void wsl::windows::common::wslutil::PrintSystemError(_In_ HRESULT result, _Inout_ FILE* const stream)
{
    fwprintf(stream, L"%ls\n", GetSystemErrorString(result).c_str());
}

void wsl::windows::common::wslutil::PrintMessageImpl(_In_ const std::wstring& message, _In_ va_list& args, _Inout_ FILE* const stream)
{
    vfwprintf(stream, message.c_str(), args);
    fputws(L"\n", stream);
}

void wsl::windows::common::wslutil::PrintMessageImpl(_In_ const std::wstring& message, _Inout_ FILE* const stream, ...)
{
    va_list arguments{};
    va_start(arguments, stream);
    auto vaEnd = wil::scope_exit([&arguments] { va_end(arguments); });
    PrintMessageImpl(message, arguments, stream);
}

void wsl::windows::common::wslutil::PrintMessage(_In_ const std::wstring& message, _Inout_ FILE* const stream)
{
    fwprintf(stream, L"%ls\n", message.c_str());
}

void wsl::windows::common::wslutil::SetCrtEncoding(int Mode)
{
    // Configure the CRT to manipulate text as the specified mode.
    auto setMode = [](FILE* stream, int Mode) {
        const auto fileNumber = _fileno(stream);
        if (fileNumber >= 0)
        {
            WI_VERIFY(_setmode(fileNumber, Mode) != -1);
        }
    };

    setMode(stdin, Mode);
    setMode(stdout, Mode);
    setMode(stderr, Mode);

    // Set the locale to the current environment's default locale.
    WI_VERIFY(_wsetlocale(LC_ALL, L"") != NULL);
}

void wsl::windows::common::wslutil::SetThreadDescription(LPCWSTR Name)
{
    LOG_IF_FAILED(::SetThreadDescription(GetCurrentThread(), Name));
}

wil::unique_hlocal_string wsl::windows::common::wslutil::SidToString(_In_ PSID UserSid)
{
    wil::unique_hlocal_string sid;
    THROW_LAST_ERROR_IF(!ConvertSidToStringSid(UserSid, &sid));

    return sid;
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

        auto* callback = reinterpret_cast<const std::function<void(UINT, LPCWSTR)>*>(context);
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

int wsl::windows::common::wslutil::UpdatePackage(bool PreRelease, bool Repair)
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

UINT wsl::windows::common::wslutil::UpgradeViaMsi(
    _In_ LPCWSTR PackageLocation, _In_opt_ LPCWSTR ExtraArgs, _In_opt_ LPCWSTR LogFile, _In_ const std::function<void(INSTALLMESSAGE, LPCWSTR)>& Callback)
{
    WriteInstallLog(std::format("Upgrading via MSI package: {}. Args: {}", PackageLocation, ExtraArgs != nullptr ? ExtraArgs : L""));

    ConfigureMsiLogging(LogFile, Callback);

    auto result = MsiInstallProduct(PackageLocation, ExtraArgs);
    WSL_LOG(
        "MsiInstallResult",
        TraceLoggingValue(result, "result"),
        TraceLoggingValue(ExtraArgs != nullptr ? ExtraArgs : L"", "ExtraArgs"));

    WriteInstallLog(std::format("MSI upgrade result: {}", result));

    return result;
}

UINT wsl::windows::common::wslutil::UninstallViaMsi(_In_opt_ LPCWSTR LogFile, _In_ const std::function<void(INSTALLMESSAGE, LPCWSTR)>& Callback)
{
    const auto key = OpenLxssMachineKey(KEY_READ);
    const auto productCode = ReadString(key.get(), L"Msi", L"ProductCode", nullptr);

    WriteInstallLog(std::format("Uninstalling MSI package: {}", productCode));

    ConfigureMsiLogging(LogFile, Callback);

    auto result = MsiConfigureProduct(productCode.c_str(), 0, INSTALLSTATE_ABSENT);
    WSL_LOG("MsiUninstallResult", TraceLoggingValue(result, "result"));

    WriteInstallLog(std::format("MSI package uninstall result: {}", result));

    return result;
}

wil::unique_hfile wsl::windows::common::wslutil::ValidateFileSignature(LPCWSTR Path)
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

void wsl::windows::common::wslutil::WriteInstallLog(const std::string& Content)
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

winrt::Windows::Management::Deployment::PackageVolume wsl::windows::common::wslutil::GetSystemVolume()
try
{
    const auto packageManager = winrt::Windows::Management::Deployment::PackageManager();
    const auto volumes = packageManager.FindPackageVolumes();
    for (auto volume : volumes)
    {
        if (volume.IsSystemVolume())
        {
            return volume;
        }
    }

    WSL_LOG("GetSystemVolumeNotFound");
    return nullptr;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return nullptr;
}