/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    helpers.cpp

Abstract:

    This file contains helper function definitions.

--*/

#include "precomp.h"
#include "helpers.hpp"
#include "Stringify.h"
#include "svccomm.hpp"
#include "socket.hpp"
#include "hvsocket.hpp"
#include "relay.hpp"
#include "LxssMessagePort.h"
#include <gsl/algorithm>
#include <gslhelpers.h>
#include "registry.hpp"
#include "versionhelpers.h"
#include <regstr.h>

using wsl::windows::common::helpers::LaunchWslRelayFlags;

constexpr auto c_WslSupportInterfaceKey = L"Software\\Classes\\Interface\\{46f3c96d-ffa3-42f0-b052-52f5e7ecbb08}";
constexpr auto c_WslSupportInterfaceName = L"IWslSupport";

namespace {

class ProcessLauncher
{
public:
    ProcessLauncher(LPCWSTR executable) : m_executable(executable)
    {
    }

    ProcessLauncher(LPCWSTR executable, LPCWSTR commandLine) : m_executable(executable), m_commandLine(commandLine)
    {
    }

    ProcessLauncher(const ProcessLauncher&) = delete;
    ProcessLauncher& operator=(const ProcessLauncher&) = delete;

    ProcessLauncher(ProcessLauncher&& other) noexcept
    {
        *this = std::move(other);
    }

    ProcessLauncher& operator=(ProcessLauncher&& other) noexcept
    {
        std::swap(m_executable, other.m_executable);
        std::swap(m_commandLine, other.m_commandLine);
        std::swap(m_handles, other.m_handles);
        return *this;
    }

    void AddOption(LPCWSTR OptionName, LPCWSTR OptionValue = nullptr)
    {
        m_commandLine += L' ';
        m_commandLine += OptionName;
        if (OptionValue)
        {
            m_commandLine += L" ";
            m_commandLine += OptionValue;
        }
    };

    void AddGuidOption(LPCWSTR OptionName, LPCGUID Guid)
    {
        if (ARGUMENT_PRESENT(Guid))
        {
            AddOption(OptionName, wsl::shared::string::GuidToString<wchar_t>(*Guid).c_str());
        }
    };

    void AddHandleOption(LPCWSTR OptionName, HANDLE Handle)
    {
        if (ARGUMENT_PRESENT(Handle))
        {
            AddOption(OptionName, std::to_wstring(HandleToUlong(Handle)).c_str());
            m_handles.push_back(Handle);
            wsl::windows::common::helpers::SetHandleInheritable(Handle);
        }
    };

    [[nodiscard]] wil::unique_handle Launch(_In_opt_ HANDLE UserToken, _In_ bool HideWindow, _In_ bool CreateNoWindow = false) const
    {
        // If a user token was provided, create an environment block from the token.
        wsl::windows::common::helpers::unique_environment_block environmentBlock{nullptr};
        if (ARGUMENT_PRESENT(UserToken))
        {
            THROW_LAST_ERROR_IF(!CreateEnvironmentBlock(&environmentBlock, UserToken, false));
        }

        wsl::windows::common::SubProcess process(m_executable.data(), m_commandLine.data(), CREATE_UNICODE_ENVIRONMENT);

        for (const auto e : m_handles)
        {
            process.InheritHandle(e);
        }

        if (HideWindow)
        {
            process.SetShowWindow(SW_HIDE);
        }

        if (CreateNoWindow)
        {
            process.SetFlags(CREATE_NO_WINDOW);
        }

        process.SetEnvironment(environmentBlock.get());
        process.SetToken(UserToken);

        // Launch the process.
        return process.Start();
    }

private:
    std::wstring m_executable;
    std::wstring m_commandLine;
    std::vector<HANDLE> m_handles;
};

[[nodiscard]] wil::unique_handle LaunchWslHost(
    _In_opt_ LPCGUID DistroId, _In_opt_ HANDLE InteropHandle, _In_opt_ HANDLE EventHandle, _In_opt_ HANDLE ParentHandle, _In_opt_ LPCGUID VmId, _In_opt_ HANDLE UserToken)
{
    // Construct the command line.
    //
    // N.B. The two places that launch wslhost.exe are the wsl.exe the service.
    const auto path = wsl::windows::common::wslutil::GetBasePath();

    // Format the command line.
    ProcessLauncher launcher((path / L"wslhost.exe").c_str());
    launcher.AddGuidOption(wslhost::distro_id_option, DistroId);
    launcher.AddGuidOption(wslhost::vm_id_option, VmId);
    launcher.AddHandleOption(wslhost::handle_option, InteropHandle);
    launcher.AddHandleOption(wslhost::event_option, EventHandle);
    launcher.AddHandleOption(wslhost::parent_option, ParentHandle);
    return launcher.Launch(UserToken, true);
}

[[nodiscard]] wil::unique_handle LaunchWslRelay(
    _In_ wslrelay::RelayMode Mode,
    _In_opt_ HANDLE InteropHandle,
    _In_opt_ LPCGUID VmId,
    _In_opt_ HANDLE PipeHandle,
    _In_opt_ std::optional<int> Port,
    _In_opt_ HANDLE ExitEvent,
    _In_opt_ HANDLE UserToken,
    _In_ LaunchWslRelayFlags Flags)
{
    // Construct the command line.
    //
    // N.B. The two places that launch wslrelay.exe are the wsl.exe the service.
    const auto path = wsl::windows::common::wslutil::GetBasePath();

    // Format the command line.
    ProcessLauncher launcher((path / L"wslrelay.exe").c_str());
    launcher.AddOption(wslrelay::mode_option, std::to_wstring(Mode).c_str());
    launcher.AddGuidOption(wslrelay::vm_id_option, VmId);
    launcher.AddHandleOption(wslrelay::handle_option, InteropHandle);
    launcher.AddHandleOption(wslrelay::pipe_option, PipeHandle);
    launcher.AddHandleOption(wslrelay::exit_event_option, ExitEvent);
    if (Port)
    {
        launcher.AddOption(wslrelay::port_option, std::to_wstring(Port.value()).c_str());
    }

    if (WI_IsFlagSet(Flags, LaunchWslRelayFlags::DisableTelemetry))
    {
        launcher.AddOption(wslrelay::disable_telemetry_option);
    }

    if (WI_IsFlagSet(Flags, LaunchWslRelayFlags::ConnectPipe))
    {
        launcher.AddOption(wslrelay::connect_pipe_option);
    }

    return launcher.Launch(UserToken, WI_IsFlagSet(Flags, LaunchWslRelayFlags::HideWindow));
}
} // namespace

void wsl::windows::common::helpers::ConnectPipe(_In_ HANDLE Pipe, _In_ DWORD Timeout, _In_ const std::vector<HANDLE>& ExitEvents)
{
    const wil::unique_event OverlappedEvent(wil::EventOptions::ManualReset);
    OVERLAPPED Overlapped = {0};
    Overlapped.hEvent = OverlappedEvent.get();
    if (!ConnectNamedPipe(Pipe, &Overlapped))
    {
        switch (GetLastError())
        {
        case ERROR_PIPE_CONNECTED:
            break;

        case ERROR_IO_PENDING:
        {
            DWORD Bytes;
            auto Cancel = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
                CancelIoEx(Pipe, &Overlapped);
                GetOverlappedResult(Pipe, &Overlapped, &Bytes, TRUE);
            });

            std::vector<HANDLE> WaitHandles;
            WaitHandles.push_back(Overlapped.hEvent);
            for (auto ExitEvent : ExitEvents)
            {
                WaitHandles.push_back(ExitEvent);
            }

            const auto Result = WaitForMultipleObjects(gsl::narrow_cast<DWORD>(WaitHandles.size()), WaitHandles.data(), FALSE, Timeout);
            if (!ExitEvents.empty() && Result > WAIT_OBJECT_0 && Result <= WAIT_OBJECT_0 + WaitHandles.size())
            {
                THROW_HR(E_ABORT);
            }

            THROW_LAST_ERROR_IF(Result != WAIT_OBJECT_0);

            Cancel.release();
            THROW_IF_WIN32_BOOL_FALSE(GetOverlappedResult(Pipe, &Overlapped, &Bytes, FALSE));
        }

        break;

        default:
            THROW_LAST_ERROR();
        }
    }
}

std::wstring_view wsl::windows::common::helpers::ConsumeArgument(_In_ std::wstring_view CommandLine, _In_ std::wstring_view Argument)
{
    WI_ASSERT((CommandLine.size() >= Argument.size()) && (wcsncmp(CommandLine.data(), Argument.data(), Argument.size()) == 0));

    CommandLine.remove_prefix(Argument.size());
    return string::StripLeadingWhitespace(CommandLine);
}

void wsl::windows::common::helpers::CreateConsole(_In_ LPCWSTR ConsoleTitle)
{
    THROW_IF_WIN32_BOOL_FALSE(AllocConsole());
    WI_VERIFY(wsl::windows::common::helpers::ReopenStdHandles());
    if (ConsoleTitle != nullptr)
    {
        LOG_IF_WIN32_BOOL_FALSE(SetConsoleTitleW(ConsoleTitle));
    }
}

wsl::windows::common::helpers::unique_proc_attribute_list wsl::windows::common::helpers::CreateProcThreadAttributeList(_In_ DWORD AttributeCount)
{
    SIZE_T Size = 0;
    if (!InitializeProcThreadAttributeList(nullptr, AttributeCount, 0, &Size))
    {
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_INSUFFICIENT_BUFFER);
    }

    unique_proc_attribute_list List(reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(CoTaskMemAlloc(Size)));
    THROW_IF_WIN32_BOOL_FALSE(InitializeProcThreadAttributeList(List.get(), AttributeCount, 0, &Size));

    return List;
}

std::vector<gsl::byte> wsl::windows::common::helpers::GenerateConfigurationMessage(
    _In_ const std::wstring& DistributionName,
    _In_ ULONG FixedDrivesBitmap,
    _In_ ULONG DefaultUid,
    _In_ const std::string& Timezone,
    _In_ const std::wstring& Plan9SocketPath,
    _In_ ULONG FeatureFlags,
    _In_ LX_INIT_DRVFS_MOUNT DrvfsMount)
{
    auto [hostName, domainName] = filesystem::GetHostAndDomainNames();

    std::string windowsHosts;

    // If DNS tunneling is enabled, we don't need to reflect the Windows hosts file in Linux, as the
    // Windows DNS client will use the Windows hosts file for tunneled DNS requests
    if (!WI_IsFlagSet(FeatureFlags, LxInitFeatureDnsTunneling))
    {
        // Parse the Windows hosts file.
        //
        // N.B. failures generating the hosts string are non-fatal.
        try
        {

            // Parse the Windows hosts file.
            std::wstring SystemDirectory;
            THROW_IF_FAILED(wil::GetSystemDirectoryW(SystemDirectory));

            windowsHosts =
                filesystem::GetWindowsHosts(std::filesystem::path(std::move(SystemDirectory)) / L"drivers" / L"etc" / L"hosts");
        }
        CATCH_LOG()
    }

    shared::MessageWriter<LX_INIT_CONFIGURATION_INFORMATION> message(LxInitMessageInitialize);
    message->DrvFsVolumesBitmap = FixedDrivesBitmap;
    message->DrvFsDefaultOwner = DefaultUid;
    message->FeatureFlags = FeatureFlags;
    message->DrvfsMount = DrvfsMount;
    message.WriteString(message->HostnameOffset, hostName);
    message.WriteString(message->DomainnameOffset, domainName);
    message.WriteString(message->WindowsHostsOffset, windowsHosts);
    message.WriteString(message->DistributionNameOffset, DistributionName);
    message.WriteString(message->Plan9SocketOffset, Plan9SocketPath);
    message.WriteString(message->TimezoneOffset, Timezone);

    return message.MoveBuffer();
}

std::vector<gsl::byte> wsl::windows::common::helpers::GenerateTimezoneUpdateMessage(_In_ std::string_view Timezone)
{
    // Construct the timezone update message.
    shared::MessageWriter<LX_INIT_TIMEZONE_INFORMATION> message(LxInitMessageTimezoneInformation);
    message.WriteString(message->TimezoneOffset, Timezone);

    return message.MoveBuffer();
}

std::string wsl::windows::common::helpers::GetLinuxTimezone(_In_opt_ HANDLE UserToken)
{
    std::string timezone{};
    try
    {
        // If a user token was specified, impersonate to get the per-user region settings.
        auto runAsSelf = UserToken ? wil::impersonate_token(UserToken) : wil::run_as_self();

        // Query the system region.
        std::vector<WCHAR> geoName;
        int length;
        do
        {
            length = GetUserDefaultGeoName(nullptr, 0);
            THROW_LAST_ERROR_IF(length == 0);

            geoName.resize(length + 1);
            length = GetUserDefaultGeoName(geoName.data(), length);
        } while ((length == 0) && (GetLastError() == ERROR_INSUFFICIENT_BUFFER));

        THROW_LAST_ERROR_IF(length == 0);

        const auto region = wsl::shared::string::WideToMultiByte(geoName.data());

        // Query the Windows timezone.
        DYNAMIC_TIME_ZONE_INFORMATION zoneInfo;
        THROW_LAST_ERROR_IF(GetDynamicTimeZoneInformation(&zoneInfo) == TIME_ZONE_ID_INVALID);

        UErrorCode status = U_ZERO_ERROR;
        auto windowsId = reinterpret_cast<const UChar*>(zoneInfo.TimeZoneKeyName);
        const auto size = ucal_getTimeZoneIDForWindowsID(windowsId, -1, region.c_str(), nullptr, 0, &status);

        // If no mapping exists, return an empty string.
        THROW_HR_IF_MSG(
            E_UNEXPECTED,
            size == 0,
            "GetTimeZoneIDForWindowsID(%ls, -1, %hs, nullptr, 0, &status) returned %d",
            zoneInfo.TimeZoneKeyName,
            region.c_str(),
            status);

        WI_ASSERT(status == UErrorCode::U_BUFFER_OVERFLOW_ERROR);

        std::vector<UChar> buffer(size + 1);
        status = U_ZERO_ERROR;
        WI_VERIFY(ucal_getTimeZoneIDForWindowsID(windowsId, -1, region.c_str(), buffer.data(), size, &status) == size);

        THROW_HR_IF_MSG(E_FAIL, (U_FAILURE(status) != false), "%hs", u_errorName(status));

        timezone.resize(buffer.size());
        u_UCharsToChars(buffer.data(), timezone.data(), static_cast<int32_t>(timezone.size()));
    }
    CATCH_LOG()

    return timezone;
}

wsl::windows::common::helpers::WindowsVersion wsl::windows::common::helpers::GetWindowsVersion()
{
    static WindowsVersion version;
    static std::once_flag flag;
    std::call_once(flag, [&]() {
        const auto regKey = registry::OpenKey(HKEY_LOCAL_MACHINE, REGSTR_PATH_NT_CURRENTVERSION, KEY_READ);
        const auto majorVersion = registry::ReadDword(regKey.get(), nullptr, L"CurrentMajorVersionNumber", 0);
        const auto minorVersion = registry::ReadDword(regKey.get(), nullptr, L"CurrentMinorVersionNumber", 0);
        const auto buildNumberString = registry::ReadString(regKey.get(), nullptr, REGSTR_VAL_CURRENT_BUILD, L"0");
        const auto buildNumber = wcstoul(buildNumberString.c_str(), nullptr, 10);
        const auto revision = registry::ReadDword(regKey.get(), nullptr, L"UBR", 0);
        version = {majorVersion, minorVersion, buildNumber, revision};
    });

    return version;
}

std::wstring wsl::windows::common::helpers::GetUniquePipeName()
{
    GUID pipeId;
    THROW_IF_FAILED(CoCreateGuid(&pipeId));
    return wslutil::ConstructPipePath(wsl::shared::string::GuidToString<wchar_t>(pipeId));
}

std::filesystem::path wsl::windows::common::helpers::GetUserProfilePath(_In_opt_ HANDLE userToken)
{
    if (userToken != nullptr)
    {
        // N.B. stringSize includes the null terminator.
        DWORD stringSize = 0;
        ::GetUserProfileDirectoryW(userToken, nullptr, &stringSize);
        WI_ASSERT(stringSize > 0);

        std::wstring path(stringSize - 1, L'\0');
        THROW_IF_WIN32_BOOL_FALSE(::GetUserProfileDirectoryW(userToken, path.data(), &stringSize));

        return std::filesystem::path(std::move(path));
    }
    else
    {
        wil::unique_cotaskmem_string profileDir;
        THROW_IF_FAILED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profileDir));

        return std::filesystem::path(profileDir.get());
    }
}

std::string wsl::windows::common::helpers::GetWindowsVersionString()
{
    std::string versionString{};
    try
    {
        const auto version = GetWindowsVersion();
        std::stringstream stream;
        stream << version.MajorVersion << "." << version.MinorVersion << "." << version.BuildNumber << "." << version.UpdateBuildRevision;
        versionString = stream.str();
    }
    CATCH_LOG()

    return versionString;
}

std::filesystem::path wsl::windows::common::helpers::GetWslConfigPath(_In_opt_ HANDLE userToken)
{
    return wsl::windows::common::helpers::GetUserProfilePath(userToken) / L".wslconfig";
}

bool wsl::windows::common::helpers::IsPackageInstalled(_In_ LPCWSTR PackageFamilyName)
{
    UINT32 packageCount = 0;
    UINT32 bufferSize = 0;
    const auto result = GetPackagesByPackageFamily(PackageFamilyName, &packageCount, nullptr, &bufferSize, nullptr);

    THROW_HR_IF(HRESULT_FROM_WIN32(result), result != ERROR_INSUFFICIENT_BUFFER && result != STATUS_SUCCESS && result != STATUS_NOT_FOUND);

    return result != STATUS_NOT_FOUND && packageCount > 0;
}

bool wsl::windows::common::helpers::IsServicePresent(_In_ LPCWSTR ServiceName)
{
    const wil::unique_schandle manager{OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT)};
    THROW_LAST_ERROR_IF(!manager);

    const wil::unique_schandle service{OpenService(manager.get(), ServiceName, SERVICE_QUERY_CONFIG)};
    return !!service;
}

bool wsl::windows::common::helpers::IsWindows11OrAbove()
{
    return GetWindowsVersion().BuildNumber >= WindowsBuildNumbers::Cobalt;
}

bool wsl::windows::common::helpers::IsWslOptionalComponentPresent()
{
    // Query if the lxss service (the lxss.sys driver) is present.
    return IsServicePresent(L"lxss");
}

bool wsl::windows::common::helpers::IsWslSupportInterfacePresent()
{
    // Check if the IWslSupport interface is registered. This interface is present on all Windows builds
    // that support the lifted WSL package.
    wil::unique_hkey key;
    try
    {
        key = windows::common::registry::OpenKey(HKEY_LOCAL_MACHINE, c_WslSupportInterfaceKey, KEY_READ);
        WI_ASSERT(windows::common::registry::ReadString(key.get(), nullptr, nullptr, nullptr) == c_WslSupportInterfaceName);
    }
    CATCH_LOG()

    return !!key;
}

void wsl::windows::common::helpers::LaunchDebugConsole(
    _In_ LPCWSTR PipeName, _In_ bool ConnectExistingPipe, _In_ HANDLE UserToken, _In_opt_ HANDLE LogFile, _In_ bool DisableTelemetry)
{
    LaunchWslRelayFlags flags{};
    wil::unique_hfile pipe;
    if (ConnectExistingPipe)
    {
        // Connect to an existing pipe. The connection should be:
        //     Asynchronous (FILE_FLAG_OVERLAPPED)
        //     Anonymous (SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS)
        //         - Don't allow the pipe server to impersonate the connecting client.
        pipe.reset(CreateFileW(
            PipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr));
    }
    else
    {
        // Create a new pipe server the child process will connect to. The pipe should be:
        //     Bi-directional: PIPE_ACCESS_DUPLEX
        //     Asynchronous: FILE_FLAG_OVERLAPPED
        //     Raw: PIPE_TYPE_BYTE | PIPE_READMODE_BYTE
        //     Blocking: PIPE_WAIT
        WI_SetFlag(flags, LaunchWslRelayFlags::ConnectPipe);
        pipe.reset(CreateNamedPipeW(
            PipeName, (PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED), (PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT), 1, LX_RELAY_BUFFER_SIZE, LX_RELAY_BUFFER_SIZE, 0, nullptr));
    }

    THROW_LAST_ERROR_IF(!pipe);

    WI_SetFlagIf(flags, LaunchWslRelayFlags::DisableTelemetry, DisableTelemetry);
    wil::unique_handle info{LaunchWslRelay(wslrelay::RelayMode::DebugConsole, LogFile, nullptr, pipe.get(), {}, nullptr, UserToken, flags)};
}

[[nodiscard]] wil::unique_handle wsl::windows::common::helpers::LaunchInteropServer(
    _In_opt_ LPCGUID DistroId, _In_ HANDLE InteropHandle, _In_opt_ HANDLE EventHandle, _In_opt_ HANDLE ParentHandle, _In_opt_ LPCGUID VmId, _In_opt_ HANDLE UserToken)
{
    return LaunchWslHost(DistroId, InteropHandle, EventHandle, ParentHandle, VmId, UserToken);
}

void wsl::windows::common::helpers::LaunchKdRelay(_In_ LPCWSTR PipeName, _In_ HANDLE UserToken, _In_ int Port, _In_ HANDLE ExitEvent, _In_ bool DisableTelemetry)
{
    // Create a new pipe server. The pipe should be:
    //     Bi-directional: PIPE_ACCESS_DUPLEX
    //     Asynchronous: FILE_FLAG_OVERLAPPED
    //     Raw: PIPE_TYPE_BYTE | PIPE_READMODE_BYTE
    //     Blocking: PIPE_WAIT
    const wil::unique_hfile pipe{CreateNamedPipeW(
        PipeName, (PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED), (PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT), 1, LX_RELAY_BUFFER_SIZE, LX_RELAY_BUFFER_SIZE, 0, nullptr)};

    THROW_LAST_ERROR_IF(!pipe);

    LaunchWslRelayFlags flags = LaunchWslRelayFlags::ConnectPipe;
    WI_SetFlagIf(flags, LaunchWslRelayFlags::DisableTelemetry, DisableTelemetry);
    wil::unique_handle info{LaunchWslRelay(wslrelay::RelayMode::KdRelay, nullptr, nullptr, pipe.get(), Port, ExitEvent, UserToken, flags)};
}

void wsl::windows::common::helpers::LaunchPortRelay(_In_ SOCKET Socket, _In_ const GUID& VmId, _In_ HANDLE UserToken, _In_ bool DisableTelemetry)
{
    LaunchWslRelayFlags flags{};
    WI_SetFlagIf(flags, LaunchWslRelayFlags::DisableTelemetry, DisableTelemetry);
    wil::unique_handle info{LaunchWslRelay(
        wslrelay::RelayMode::PortRelay, reinterpret_cast<HANDLE>(Socket), &VmId, nullptr, {}, nullptr, UserToken, flags)};
}

void wsl::windows::common::helpers::LaunchWslSettingsOOBE(_In_ HANDLE UserToken)
{
    const auto wslSettingsExePath = wsl::windows::common::wslutil::GetBasePath() / L"wslsettings" / L"wslsettings.exe";
    static constexpr auto commandLine = L" ----ms-protocol:wsl-settings://oobe";

    wsl::windows::common::SubProcess process(wslSettingsExePath.c_str(), commandLine);
    process.SetToken(UserToken);
    process.SetShowWindow(SW_SHOW);

    wsl::windows::common::helpers::unique_environment_block environmentBlock{nullptr};
    THROW_LAST_ERROR_IF(!CreateEnvironmentBlock(&environmentBlock, UserToken, false));

    process.SetEnvironment(environmentBlock.get());

    process.Start();
}

std::wstring_view wsl::windows::common::helpers::ParseArgument(_In_ std::wstring_view CommandLine, _In_ bool HandleQuotes)
{
    std::wstring_view Argument = CommandLine;
    const size_t Index = Argument.find_first_of(L" \t");
    if (Index != std::wstring_view::npos)
    {
        Argument = Argument.substr(0, Index);
    }

    if (HandleQuotes && CommandLine.find_first_of(L"\"") == 0)
    {
        const auto QuoteIndex = CommandLine.find_first_of(L"\"", 1);
        if (QuoteIndex != std::wstring_view::npos)
        {
            Argument = CommandLine.substr(0, QuoteIndex + 1);
        }
    }

    return Argument;
}

bool wsl::windows::common::helpers::ReopenStdHandles()
{
    // Reopen the standard streams to make sure *printf* methods will write to the correct place.
    if (_wfreopen(L"CONIN$", L"r", stdin) == nullptr || _wfreopen(L"CONOUT$", L"w", stdout) == nullptr ||
        _wfreopen(L"CONOUT$", L"w", stderr) == nullptr)
    {
        return false;
    }

    // Configure std::cout, std::cerr and std::cin to use the reopened FILE*.
    std::ios::sync_with_stdio();

    return true;
}

#ifdef _WIN64
INT64
wsl::windows::common::helpers::RoundUpToNearestPowerOfTwo(_In_ INT64 Num)
#else
INT32
wsl::windows::common::helpers::RoundUpToNearestPowerOfTwo(_In_ INT32 Num)
#endif
{
    // Don't round the number up further if it's zero or already a power of two.
    if (Num == 0 || (Num & (Num - 1)) == 0)
    {
        return Num;
    }

    // Round the number up to the nearest power of two.
#ifdef _WIN64
    ULONG index = 0;
    WI_VERIFY(_BitScanReverse64(&index, Num));

    return 1i64 << (index + 1);
#else
    ULONG index = 0;
    WI_VERIFY(_BitScanReverse(&index, Num));

    return 1i32 << (index + 1);
#endif
}

DWORD
wsl::windows::common::helpers::RunProcess(_Inout_ std::wstring& CommandLine)
{
    SubProcess process(nullptr, CommandLine.c_str());
    return process.Run();
}

void wsl::windows::common::helpers::SetHandleInheritable(_In_ HANDLE Handle, _In_ bool Inheritable)
{
    THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(Handle, HANDLE_FLAG_INHERIT, Inheritable ? HANDLE_FLAG_INHERIT : 0));
}

bool wsl::windows::common::helpers::TryAttachConsole()
{
    if (!AttachConsole(GetCurrentProcessId()) && !AttachConsole(ATTACH_PARENT_PROCESS))
    {
        return false;
    }

    return ReopenStdHandles();
}
