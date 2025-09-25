/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DllMain.cpp

Abstract:

    This file the entrypoint for the WSLA client library.

--*/

#include "precomp.h"
#include "wslservice.h"
#include "WSLAApi.h"
#include "wslrelay.h"
#include "wslInstall.h"

namespace {

void ConfigureComSecurity(IUnknown* Instance)
{
    wil::com_ptr_nothrow<IClientSecurity> clientSecurity;
    THROW_IF_FAILED(Instance->QueryInterface(IID_PPV_ARGS(&clientSecurity)));

    // Get the current proxy blanket settings.
    DWORD authnSvc, authzSvc, authnLvl, capabilites;
    THROW_IF_FAILED(clientSecurity->QueryBlanket(Instance, &authnSvc, &authzSvc, NULL, &authnLvl, NULL, NULL, &capabilites));

    // Make sure that dynamic cloaking is used.
    WI_ClearFlag(capabilites, EOAC_STATIC_CLOAKING);
    WI_SetFlag(capabilites, EOAC_DYNAMIC_CLOAKING);
    THROW_IF_FAILED(clientSecurity->SetBlanket(Instance, authnSvc, authzSvc, NULL, authnLvl, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, capabilites));
}
} // namespace

class DECLSPEC_UUID("7BC4E198-6531-4FA6-ADE2-5EF3D2A04DFF") CallbackInstance
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ITerminationCallback, IFastRundown>
{

public:
    CallbackInstance(WslVirtualMachineTerminationCallback callback, void* context) : m_callback(callback), m_context(context)
    {
    }

    HRESULT OnTermination(ULONG Reason, LPCWSTR Details) override
    {
        return m_callback(m_context, static_cast<WslVirtualMachineTerminationReason>(Reason), Details);
    }

private:
    WslVirtualMachineTerminationCallback m_callback = nullptr;
    void* m_context = nullptr;
};

HRESULT WslGetVersion(WSL_VERSION_INFORMATION* Version)
try
{
    wil::com_ptr<IWSLAUserSession> session;

    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&session)));

    static_assert(sizeof(WSL_VERSION_INFORMATION) == sizeof(WSL_VERSION));

    return session->GetVersion(reinterpret_cast<WSL_VERSION*>(Version));
}
CATCH_RETURN();

HRESULT WslCreateVirtualMachine(const WslVirtualMachineSettings* UserSettings, WslVirtualMachineHandle* VirtualMachine)
try
{
    wil::com_ptr<IWSLAUserSession> session;

    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&session)));
    ConfigureComSecurity(session.get());

    wil::com_ptr<IWSLAVirtualMachine> virtualMachineInstance;

    VIRTUAL_MACHINE_SETTINGS settings{};
    settings.DisplayName = UserSettings->DisplayName;
    settings.MemoryMb = UserSettings->Memory.MemoryMb;
    settings.CpuCount = UserSettings->CPU.CpuCount;
    settings.BootTimeoutMs = UserSettings->Options.BootTimeoutMs;
    settings.DmesgOutput = HandleToULong(UserSettings->Options.Dmesg);
    settings.EnableDebugShell = UserSettings->Options.EnableDebugShell;
    settings.EnableEarlyBootDmesg = UserSettings->Options.EnableEarlyBootDmesg;
    settings.NetworkingMode = UserSettings->Networking.Mode;
    settings.EnableDnsTunneling = UserSettings->Networking.DnsTunneling;
    settings.EnableGPU = UserSettings->GPU.Enable;

    THROW_IF_FAILED(session->CreateVirtualMachine(&settings, &virtualMachineInstance));
    ConfigureComSecurity(virtualMachineInstance.get());

    // Register termination callback, if specified
    if (UserSettings->Options.TerminationCallback != nullptr)
    {
        auto callbackInstance =
            wil::MakeOrThrow<CallbackInstance>(UserSettings->Options.TerminationCallback, UserSettings->Options.TerminationContext);

        THROW_IF_FAILED(virtualMachineInstance->RegisterCallback(callbackInstance.Get()));

        // Callback instance is now owned by the service.
    }

    *reinterpret_cast<IWSLAVirtualMachine**>(VirtualMachine) = virtualMachineInstance.detach();
    return S_OK;
}
CATCH_RETURN();

HRESULT WslAttachDisk(WslVirtualMachineHandle VirtualMachine, const WslDiskAttachSettings* Settings, WslAttachedDiskInformation* AttachedDisk)
{
    wil::unique_cotaskmem_ansistring device;
    RETURN_IF_FAILED(reinterpret_cast<IWSLAVirtualMachine*>(VirtualMachine)
                         ->AttachDisk(Settings->WindowsPath, Settings->ReadOnly, &device, &AttachedDisk->ScsiLun));

    auto deviceSize = strlen(device.get());
    WI_VERIFY(deviceSize < sizeof(WslAttachedDiskInformation::Device));

    strncpy(AttachedDisk->Device, device.get(), sizeof(WslAttachedDiskInformation::Device));

    return S_OK;
}

HRESULT WslMount(WslVirtualMachineHandle VirtualMachine, const WslMountSettings* Settings)
{
    return reinterpret_cast<IWSLAVirtualMachine*>(VirtualMachine)
        ->Mount(Settings->Device, Settings->Target, Settings->Type, Settings->Options, Settings->Flags);
}

HRESULT WslCreateLinuxProcess(WslVirtualMachineHandle VirtualMachine, WslCreateProcessSettings* UserSettings, int32_t* Pid)
{
    WSLA_CREATE_PROCESS_OPTIONS options{};

    auto Count = [](const auto* Ptr) -> ULONG {
        if (Ptr == nullptr)
        {
            return 0;
        }

        ULONG Result = 0;

        while (*Ptr != nullptr)
        {
            Result++;
            Ptr++;
        }

        return Result;
    };

    options.Executable = UserSettings->Executable;
    options.CommandLine = UserSettings->Arguments;
    options.CommandLineCount = Count(options.CommandLine);
    options.Environment = UserSettings->Environment;
    options.EnvironmentCount = Count(options.Environment);
    options.CurrentDirectory = UserSettings->CurrentDirectory;

    WSLA_CREATE_PROCESS_RESULT result{};

    std::vector<WSLA_PROCESS_FD> inputFd(UserSettings->FdCount);
    for (size_t i = 0; i < UserSettings->FdCount; i++)
    {
        inputFd[i] = {
            UserSettings->FileDescriptors[i].Number,
            UserSettings->FileDescriptors[i].Type,
            (char*)UserSettings->FileDescriptors[i].Path};
    }

    std::vector<ULONG> fds(UserSettings->FdCount);
    if (fds.empty())
    {
        fds.resize(1); // COM doesn't like null pointers.
    }

    RETURN_IF_FAILED(reinterpret_cast<IWSLAVirtualMachine*>(VirtualMachine)
                         ->CreateLinuxProcess(&options, UserSettings->FdCount, inputFd.data(), fds.data(), &result));

    for (size_t i = 0; i < UserSettings->FdCount; i++)
    {
        UserSettings->FileDescriptors[i].Handle = UlongToHandle(fds[i]);
    }

    *Pid = result.Pid;

    return S_OK;
}

HRESULT WslWaitForLinuxProcess(WslVirtualMachineHandle VirtualMachine, int32_t Pid, uint64_t TimeoutMs, WslWaitResult* Result)
{
    static_assert(WslProcessStateUnknown == WSLAOpenFlagsUnknown);
    static_assert(WslProcessStateRunning == WSLAOpenFlagsRunning);
    static_assert(WslProcessStateExited == WSLAOpenFlagsExited);
    static_assert(WslProcessStateSignaled == WSLAOpenFlagsSignaled);
    static_assert(sizeof(WslProcessState) == sizeof(WSLAOpenFlags));
    static_assert(sizeof(WslProcessState) == sizeof(ULONG));

    return reinterpret_cast<IWSLAVirtualMachine*>(VirtualMachine)
        ->WaitPid(Pid, TimeoutMs, reinterpret_cast<ULONG*>(&Result->State), &Result->Code);
}

HRESULT WslSignalLinuxProcess(WslVirtualMachineHandle VirtualMachine, int32_t Pid, int32_t Signal)
{
    return reinterpret_cast<IWSLAVirtualMachine*>(VirtualMachine)->Signal(Pid, Signal);
}

HRESULT WslShutdownVirtualMachine(WslVirtualMachineHandle VirtualMachine, uint64_t TimeoutMs)
{
    return reinterpret_cast<IWSLAVirtualMachine*>(VirtualMachine)->Shutdown(TimeoutMs);
}

void WslReleaseVirtualMachine(WslVirtualMachineHandle VirtualMachine)
{
    reinterpret_cast<IWSLAVirtualMachine*>(VirtualMachine)->Release();
}

HRESULT WslMapPort(WslVirtualMachineHandle VirtualMachine, const WslPortMappingSettings* UserSettings)
{
    return reinterpret_cast<IWSLAVirtualMachine*>(VirtualMachine)
        ->MapPort(UserSettings->AddressFamily, UserSettings->WindowsPort, UserSettings->LinuxPort, false);
}

HRESULT WslUnmapPort(WslVirtualMachineHandle VirtualMachine, const WslPortMappingSettings* UserSettings)
{
    return reinterpret_cast<IWSLAVirtualMachine*>(VirtualMachine)
        ->MapPort(UserSettings->AddressFamily, UserSettings->WindowsPort, UserSettings->LinuxPort, true);
}

HRESULT WslLaunchInteractiveTerminal(HANDLE Input, HANDLE Output, HANDLE* Process)
try
{
    wsl::windows::common::helpers::SetHandleInheritable(Input);
    wsl::windows::common::helpers::SetHandleInheritable(Output);

    auto basePath = wsl::windows::common::wslutil::GetMsiPackagePath();
    THROW_HR_IF(E_UNEXPECTED, !basePath.has_value());

    auto commandLine = std::format(
        L"{}/wslrelay.exe --mode {} --input {} --output {}",
        basePath.value(),
        static_cast<int>(wslrelay::RelayMode::InteractiveConsoleRelay),
        HandleToULong(Input),
        HandleToULong(Output));

    WSL_LOG("LaunchWslRelay", TraceLoggingValue(commandLine.c_str(), "cmd"));

    wsl::windows::common::SubProcess process{nullptr, commandLine.c_str()};
    process.InheritHandle(Input);
    process.InheritHandle(Output);
    process.SetFlags(CREATE_NEW_CONSOLE);
    process.SetShowWindow(SW_SHOW);
    *Process = process.Start().release();

    return S_OK;
}
CATCH_RETURN();

HRESULT WslLaunchDebugShell(WslVirtualMachineHandle VirtualMachine, HANDLE* Process)
try
{
    wil::unique_cotaskmem_string pipePath;
    THROW_IF_FAILED(reinterpret_cast<IWSLAVirtualMachine*>(VirtualMachine)->GetDebugShellPipe(&pipePath));

    wil::unique_hfile pipe{CreateFileW(pipePath.get(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};
    THROW_LAST_ERROR_IF(!pipe);

    wsl::windows::common::helpers::SetHandleInheritable(pipe.get());

    auto basePath = wsl::windows::common::wslutil::GetMsiPackagePath();
    THROW_HR_IF(E_UNEXPECTED, !basePath.has_value());
    auto commandLine = std::format(
        L"\"{}\\wslrelay.exe\" --mode {} --input {} --output {}",
        basePath.value(),
        static_cast<int>(wslrelay::RelayMode::InteractiveConsoleRelay),
        HandleToULong(pipe.get()),
        HandleToULong(pipe.get()));

    WSL_LOG("LaunchDebugShellRelay", TraceLoggingValue(commandLine.c_str(), "cmd"));

    wsl::windows::common::SubProcess process{nullptr, commandLine.c_str()};
    process.InheritHandle(pipe.get());
    process.SetFlags(0);
    process.SetShowWindow(SW_SHOW);

    *Process = process.Start().release();

    return S_OK;
}
CATCH_RETURN();

HRESULT WslUnmount(WslVirtualMachineHandle VirtualMachine, const char* Path)
{
    return reinterpret_cast<IWSLAVirtualMachine*>(VirtualMachine)->Unmount(Path);
}

HRESULT WslDetachDisk(WslVirtualMachineHandle VirtualMachine, ULONG Lun)
{
    return reinterpret_cast<IWSLAVirtualMachine*>(VirtualMachine)->DetachDisk(Lun);
}
EXTERN_C BOOL STDAPICALLTYPE DllMain(_In_ HINSTANCE Instance, _In_ DWORD Reason, _In_opt_ LPVOID Reserved)
{
    wil::DLLMain(Instance, Reason, Reserved);

    switch (Reason)
    {
    case DLL_PROCESS_ATTACH:
        WslTraceLoggingInitialize(LxssTelemetryProvider, false);
        wsl::windows::common::wslutil::InitializeWil();

        break;

    case DLL_PROCESS_DETACH:
        WslTraceLoggingUninitialize();
        break;
    }

    return TRUE;
}

DEFINE_ENUM_FLAG_OPERATORS(WslInstallComponent);

HRESULT WslQueryMissingComponents(enum WslInstallComponent* Components)
try
{
    *Components = WslInstallComponentNone;

    // Check for Windows features
    WI_SetFlagIf(
        *Components,
        WslInstallComponentWslOC,
        !wsl::windows::common::helpers::IsWindows11OrAbove() && !wsl::windows::common::helpers::IsServicePresent(L"lxssmanager"));

    WI_SetFlagIf(*Components, WslInstallComponentVMPOC, !wsl::windows::common::wslutil::IsVirtualMachinePlatformInstalled());

    // Check if the WSL package is installed, and if the version supports WSLA
    auto version = wsl::windows::common::wslutil::GetInstalledPackageVersion();

    constexpr auto minimalPackageVersion = std::tuple{2, 7, 0};
    WI_SetFlagIf(*Components, WslInstallComponentWslPackage, !version.has_value() || version < minimalPackageVersion);

    // TODO: Check if hardware supports virtualization.

    return S_OK;
}
CATCH_RETURN();

// Used for debugging.
static LPCWSTR PackageUrl = nullptr;

HRESULT WslSetPackageUrl(LPCWSTR Url)
{
    PackageUrl = Url;
    return S_OK;
}

HRESULT WslInstallComponents(enum WslInstallComponent Components, WslInstallCallback ProgressCallback, void* Context)
try
{
    // Check for invalid flags.
    RETURN_HR_IF_MSG(
        E_INVALIDARG,
        (Components & ~(WslInstallComponentVMPOC | WslInstallComponentWslOC | WslInstallComponentWslPackage)) != 0,
        "Unexpected flag: %i",
        Components);

    // Fail if the caller is not elevated.
    RETURN_HR_IF(
        HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED),
        Components != 0 && !wsl::windows::common::security::IsTokenElevated(wil::open_current_access_token().get()));

    if (WI_IsFlagSet(Components, WslInstallComponentWslPackage))
    {
        THROW_HR_IF(E_INVALIDARG, PackageUrl == nullptr);

        auto callback = [&](uint64_t progress, uint64_t total) {
            if (ProgressCallback != nullptr)
            {
                ProgressCallback(WslInstallComponentWslPackage, progress, total, Context);
            }
        };

        const auto downloadPath = wsl::windows::common::wslutil::DownloadFileImpl(PackageUrl, L"wsl.msi", callback);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove(downloadPath); });

        auto exitCode = wsl::windows::common::wslutil::UpgradeViaMsi(downloadPath.c_str(), nullptr, nullptr, [](auto, auto) {});
        THROW_HR_IF_MSG(
            E_FAIL, exitCode != 0, "MSI installation failed. URL: %ls, DownloadPath: %ls, exitCode: %u", downloadPath.c_str(), PackageUrl, exitCode);
    }

    std::vector<std::wstring> optionalComponents;
    if (WI_IsFlagSet(Components, WslInstallComponentWslOC))
    {
        if (ProgressCallback != nullptr)
        {
            ProgressCallback(WslInstallComponentWslOC, 0, 1, Context);
        }

        auto exitCode = WslInstall::InstallOptionalComponent(WslInstall::c_optionalFeatureNameWsl, false);
        THROW_HR_IF_MSG(E_FAIL, exitCode != 0 && exitCode != ERROR_SUCCESS_REBOOT_REQUIRED, "Failed to install '%ls', %lu", WslInstall::c_optionalFeatureNameWsl, exitCode);
    }

    if (WI_IsFlagSet(Components, WslInstallComponentVMPOC))
    {
        if (ProgressCallback != nullptr)
        {
            ProgressCallback(WslInstallComponentVMPOC, 0, 1, Context);
        }

        auto exitCode = WslInstall::InstallOptionalComponent(WslInstall::c_optionalFeatureNameVmp, false);
        THROW_HR_IF_MSG(E_FAIL, exitCode != 0 && exitCode != ERROR_SUCCESS_REBOOT_REQUIRED, "Failed to install '%ls', %lu", WslInstall::c_optionalFeatureNameVmp, exitCode);
    }

    return WI_IsAnyFlagSet(Components, WslInstallComponentWslOC | WslInstallComponentVMPOC)
               ? HRESULT_FROM_WIN32(ERROR_SUCCESS_REBOOT_REQUIRED)
               : S_OK;
}
CATCH_RETURN();

HRESULT WslMountWindowsFolder(WslVirtualMachineHandle VirtualMachine, LPCWSTR WindowsPath, const char* Target, BOOL ReadOnly)
try
{
    return reinterpret_cast<IWSLAVirtualMachine*>(VirtualMachine)->MountWindowsFolder(WindowsPath, Target, ReadOnly);
}
CATCH_RETURN();

HRESULT WslUnmountWindowsFolder(WslVirtualMachineHandle VirtualMachine, const char* Target)
try
{
    return reinterpret_cast<IWSLAVirtualMachine*>(VirtualMachine)->UnmountWindowsFolder(Target);
}
CATCH_RETURN();

HRESULT WslMountGpuLibraries(WslVirtualMachineHandle VirtualMachine, const char* LibrariesMountPoint, const char* DriversMountpoint, WslMountFlags Flags)
try
{
    return reinterpret_cast<IWSLAVirtualMachine*>(VirtualMachine)->MountGpuLibraries(LibrariesMountPoint, DriversMountpoint, static_cast<DWORD>(Flags));
}
CATCH_RETURN();