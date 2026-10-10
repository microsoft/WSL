/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslCoreVm.cpp

Abstract:

    This file contains utility VM function definitions.

--*/

#include "precomp.h"
#include "WslCoreVm.h"
#include "HcsVirtualMachineBackend.h"
#include "GuestDeviceManager.h"
#include "WslCoreNetworkingSupport.h"
#include <lxfsshares.h>
#include "disk.hpp"
#include "WslCoreInstance.h"
#include "NatNetworking.h"
#include "OpenVmmNatNetworking.h"
#include "BridgedNetworking.h"
#include "MirroredNetworking.h"
#include "WslCoreFirewallSupport.h"
#include "DnsResolver.h"
#include "ConsommeNetworking.h"

#include <TraceLoggingProvider.h>

using msl::utilities::SafeInt;
using wsl::windows::common::helpers::WindowsBuildNumbers;
using namespace wsl::windows::common::registry;
using namespace wsl::windows::common::string;
using namespace std::string_literals;

// The default high-gap MMIO space is 16GB
#define DEFAULT_HIGH_MMIO_GAP_IN_MB (16 * _1KB)

// Start of unaddressable memory if guest only supports the minimum 36-bit addressing.
#define MAX_36_BIT_PAGE_IN_MB (0x1000000000 / _1MB)

#define WSLG_SHARED_MEMORY_SIZE_MB 8192
#define PAGE_SIZE 0x1000

static constexpr size_t c_bootEntropy = 0x1000;
static constexpr auto c_localDevicesKey = L"SOFTWARE\\Microsoft\\Terminal Server Client\\LocalDevices";

#define LXSS_ENABLE_GUI_APPS() (m_vmConfig.EnableGuiApps && (m_systemDistroDeviceId != ULONG_MAX))

using namespace wsl::windows::common;
using wsl::core::NetworkingMode;
using wsl::core::networking::NetworkEndpoint;
using wsl::core::networking::NetworkSettings;
using wsl::shared::Localization;
using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;

namespace {
INT64
RequiredExtraMmioSpaceForPmemFileInMb(_In_ PCWSTR FilePath)
{
    // Open the file and retrieve the file's size.
    const wil::unique_hfile fileHandle{CreateFile(FilePath, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
    THROW_LAST_ERROR_IF(!fileHandle);

    LARGE_INTEGER fileSizeBytes;
    THROW_IF_WIN32_BOOL_FALSE(GetFileSizeEx(fileHandle.get(), &fileSizeBytes));

    // The file is mapped to the VM using PCI BARs, which can only be a power of two. Therefore,
    // round the file size up to the nearest power of two.
    fileSizeBytes.QuadPart = wsl::windows::common::helpers::RoundUpToNearestPowerOfTwo(fileSizeBytes.QuadPart);

    // Convert from bytes to megabytes. Ensure that we don't truncate a 512kb file to 0mb.
    return std::max(fileSizeBytes.QuadPart / static_cast<INT64>(_1MB), 1i64);
}

LX_MINI_INIT_MOUNT_DEVICE_TYPE ToMiniInitDeviceType(VmDiskTransport Transport)
{
    switch (Transport)
    {
    case VmDiskTransport::Scsi:
        return LxMiniInitMountDeviceTypeScsi;
    case VmDiskTransport::VirtioBlk:
        return LxMiniInitMountDeviceTypeVirtioBlk;
    default:
        FAIL_FAST();
    }
}

VmDiskAttachment FindAttachedDiskByLun(IVirtualMachineBackend& Backend, ULONG Lun)
{
    const auto disks = Backend.GetAttachedDisks();
    const auto disk = std::ranges::find_if(disks, [&](const auto& candidate) { return candidate.GuestAddress.Lun == Lun; });
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), disk == disks.end());
    return *disk;
}
} // namespace

WslCoreVm::WslCoreVm(_In_ wsl::core::Config&& VmConfig, _In_ InitializeDrvFsCallback InitializeDrvFs) :
    m_vmConfig(std::move(VmConfig)), m_initializeDrvFs(std::move(InitializeDrvFs)), m_traceClient(m_vmConfig.EnableTelemetry)
{
    // Create a job object that will terminate child processes (wslhost.exe, wslrelay.exe)
    // when the VM is destroyed.
    m_processJobObject = wsl::windows::common::helpers::CreateKillOnCloseJob();
}

std::unique_ptr<WslCoreVm> WslCoreVm::Create(
    _In_ const wil::shared_handle& UserToken, _In_ wsl::core::Config&& VmConfig, _In_ const GUID& VmId, _In_ InitializeDrvFsCallback InitializeDrvFs)
{
    THROW_HR_IF(E_INVALIDARG, !InitializeDrvFs);

    auto newInstance = std::unique_ptr<WslCoreVm>{new WslCoreVm{std::move(VmConfig), std::move(InitializeDrvFs)}};
    try
    {
        const auto startTimeMs = GetTickCount64();
        auto privateKernel = !newInstance->m_vmConfig.KernelPath.empty();
        // Log telemetry on how long it took to create the VM
        WSL_LOG_TELEMETRY(
            "CreateVmBegin", PDT_ProductAndServicePerformance, TraceLoggingValue(VmId, "vmId"), CONFIG_TELEMETRY(newInstance->m_vmConfig));

        newInstance->Initialize(VmId, UserToken);

        const auto timeToCreateVmMs = GetTickCount64() - startTimeMs;
        WSL_LOG_TELEMETRY(
            "CreateVmEnd",
            PDT_ProductAndServicePerformance,
            TraceLoggingValue(privateKernel, "privateKernel"),
            TraceLoggingValue(newInstance->m_kernelVersionString.c_str(), "kernelVersion"),
            TraceLoggingValue(newInstance->m_runtimeId, "vmId"),
            TraceLoggingValue(timeToCreateVmMs, "timeToCreateVmMs"),
            CONFIG_TELEMETRY(newInstance->m_vmConfig));
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();

        // Log telemetry when the WSL VM fails to start including the error
        WSL_LOG_TELEMETRY(
            "FailedToStartVm",
            PDT_ProductAndServicePerformance,
            TraceLoggingValue(VmId, "vmId"),
            TraceLoggingValue(hr, "error"),
            CONFIG_TELEMETRY(newInstance->m_vmConfig));

        if (hr == HRESULT_FROM_WIN32(WSAENOTCONN) || hr == HRESULT_FROM_WIN32(WSAECONNRESET) || hr == HRESULT_FROM_WIN32(WSAETIMEDOUT))
        {
            // A kernel panic can cause an hvsocket error. If we hit this, wait one second for an HCS notification to give a better error for the user.
            if (newInstance->m_vmCrashEvent.wait(1000))
            {
                const auto crashLogPath = newInstance->m_backend ? newInstance->m_backend->GetCrashLogPath() : std::nullopt;
                if (crashLogPath)
                {
                    THROW_HR_WITH_USER_ERROR(
                        WSL_E_VM_CRASHED,
                        wsl::shared::Localization::MessageWSL2Crashed() + L"\r\n" +
                            Localization::MessageWSL2CrashedStackTrace(crashLogPath.value()));
                }
                else
                {
                    THROW_HR_WITH_USER_ERROR(WSL_E_VM_CRASHED, wsl::shared::Localization::MessageWSL2Crashed());
                }
            }
        }

        throw;
    }

    return newInstance;
}

void WslCoreVm::Initialize(const GUID& VmId, const wil::shared_handle& UserToken)
{
    auto signalEarlyTermination = wil::scope_exit([&] { m_terminatingEvent.SetEvent(); });
    const auto backendKind = SelectVirtualMachineBackendKind(m_vmConfig.EnableOpenVmm);
    ValidateBackendConfiguration(backendKind);

    // create a restricted version of the token.
    m_userToken = UserToken;
    m_restrictedToken = wsl::windows::common::security::CreateRestrictedToken(m_userToken.get());

    // Make a copy of the user sid.
    auto tokenUser = wil::get_token_information<TOKEN_USER>(m_userToken.get());
    THROW_IF_WIN32_BOOL_FALSE(::CopySid(sizeof(m_userSid), &m_userSid.Sid, tokenUser->User.Sid));

    // Generate a machine ID string based on the VM ID. This is used for some HCS APIs.
    m_machineId = wsl::shared::string::GuidToString<wchar_t>(VmId, wsl::shared::string::GuidToStringFlags::Uppercase);

    // Set the install path of the package.
    m_installPath = wsl::windows::common::wslutil::GetBasePath();

    // Initialize the path to the tools folder which also serves as the default rootfs path.
    m_rootFsPath = m_installPath / LXSS_TOOLS_DIRECTORY;

    // Store the path of the user profile.
    m_userProfile = wsl::windows::common::helpers::GetUserProfilePath(m_userToken.get());

    // Query the Windows version.
    m_windowsVersion = wsl::windows::common::helpers::GetWindowsVersion();

    // Create a temporary folder for the VM.
    try
    {
        const auto runAsUser = wil::impersonate_token(m_userToken.get());
        m_tempPath = wsl::windows::common::filesystem::GetTempFolderPath(m_userToken.get()) / m_machineId;

        wil::CreateDirectoryDeep(m_tempPath.c_str());
        m_tempDirectoryCreated = true;
    }
    CATCH_LOG();

    // If a private kernel was not specified, use the default.
    m_defaultKernel = m_vmConfig.KernelPath.empty();
    if (m_defaultKernel)
    {
#ifdef WSL_KERNEL_PATH

        m_vmConfig.KernelPath = TEXT(WSL_KERNEL_PATH);

#else

        m_vmConfig.KernelPath = m_rootFsPath / LXSS_VM_MODE_KERNEL_NAME;

#endif
    }
    else
    {
        if (!wsl::windows::common::filesystem::FileExists(m_vmConfig.KernelPath.c_str()))
        {
            THROW_HR_WITH_USER_ERROR(
                WSL_E_CUSTOM_KERNEL_NOT_FOUND,
                Localization::MessageCustomKernelNotFound(
                    wsl::windows::common::helpers::GetWslConfigPath(m_userToken.get()), m_vmConfig.KernelPath.c_str()));
        }

        // Direct boot is not supported on ARM64. Modify the rootfs directory to be a temporary directory that contains
        // copies of the initrd file and private kernel.
        if constexpr (wsl::shared::Arm64)
        {
            auto impersonate = wil::impersonate_token(m_userToken.get());

            m_rootFsPath = m_tempPath / LXSS_ROOTFS_DIRECTORY;
            wil::CreateDirectoryDeep(m_rootFsPath.c_str());
            auto initRdPath = m_installPath / LXSS_TOOLS_DIRECTORY / LXSS_VM_MODE_INITRD_NAME;

            auto targetPath = m_rootFsPath / LXSS_VM_MODE_INITRD_NAME;
            THROW_IF_WIN32_BOOL_FALSE(CopyFileW(initRdPath.c_str(), targetPath.c_str(), TRUE));

            targetPath = m_rootFsPath / LXSS_VM_MODE_KERNEL_NAME;
            THROW_IF_WIN32_BOOL_FALSE(CopyFileW(m_vmConfig.KernelPath.c_str(), targetPath.c_str(), TRUE));
        }
    }

    // If the user did not specify custom modules, use the default modules only if using the default kernel.
    m_privateKernelModules = !m_vmConfig.KernelModulesPath.empty();
    if (m_vmConfig.KernelModulesPath.empty())
    {
        if (m_defaultKernel)
        {
#ifdef WSL_KERNEL_MODULES_PATH

            m_vmConfig.KernelModulesPath = std::wstring(TEXT(WSL_KERNEL_MODULES_PATH));
            m_privateKernelModules = true;

#else

            m_vmConfig.KernelModulesPath = m_rootFsPath / L"artifacts.vhd";

#endif
        }
    }
    else
    {
        if (!wsl::windows::common::filesystem::FileExists(m_vmConfig.KernelModulesPath.c_str()))
        {
            THROW_HR_WITH_USER_ERROR(
                WSL_E_CUSTOM_KERNEL_NOT_FOUND,
                Localization::MessageCustomKernelModulesNotFound(
                    wsl::windows::common::helpers::GetWslConfigPath(m_userToken.get()), m_vmConfig.KernelModulesPath.c_str()));
        }

        if (m_defaultKernel)
        {
            THROW_HR_WITH_USER_ERROR(WSL_E_CUSTOM_KERNEL_NOT_FOUND, Localization::MessageMismatchedKernelModulesError());
        }
    }

    // If debug console was requested, create a randomly-named pipe and spawn a wslhost process to read from the pipe.
    //
    // N.B. wslhost.exe is launched at medium integrity level and its lifetime
    //      is tied to the lifetime of the utility VM.
    if (m_vmConfig.EnableDebugConsole || !m_vmConfig.DebugConsoleLogFile.empty())
    {
        try
        {
            m_vmConfig.EnableDebugConsole = true;
            m_comPipe0 = wsl::windows::common::helpers::GetUniquePipeName();
        }
        CATCH_LOG()
    }

    // If the system supports virtio console serial ports, use dmesg capture for telemetry and/or debug output.
    // Legacy serial is much slower, so this is not enabled without virtio console support.
    const auto enableVirtioSerial =
        m_vmConfig.EnableVirtio && (backendKind != BackendKind::Hcs || helpers::IsVirtioSerialConsoleSupported());
    m_vmConfig.EnableDebugShell &= enableVirtioSerial;
    if (enableVirtioSerial)
    {
        try
        {
            bool enableTelemetry = TraceLoggingProviderEnabled(g_hTraceLoggingProvider, WINEVENT_LEVEL_INFO, 0);
            m_dmesgCollector = DmesgCollector::Create(
                VmId, m_vmExitEvent.get(), enableTelemetry, m_vmConfig.EnableDebugConsole, m_comPipe0, m_vmConfig.EnableEarlyBootLogging, {});

            WSL_LOG("DMESG collector created");

            if (m_vmConfig.EnableDebugShell)
            {
                m_debugShellPipe = wsl::windows::common::wslutil::GetDebugShellPipeName(&m_userSid.Sid);
            }

            m_gnsTelemetryLogger = GuestTelemetryLogger::Create(VmId, m_vmExitEvent);
        }
        CATCH_LOG()
    }

    if (m_vmConfig.EnableDebugConsole)
    {
        try
        {
            // If specified, create a file to log the debug console output.
            wil::unique_hfile logFile;
            if (!m_vmConfig.DebugConsoleLogFile.empty())
            {
                auto impersonate = wil::impersonate_token(m_userToken.get());
                logFile.reset(CreateFileW(
                    m_vmConfig.DebugConsoleLogFile.c_str(), FILE_APPEND_DATA, (FILE_SHARE_READ | FILE_SHARE_WRITE), nullptr, OPEN_ALWAYS, 0, nullptr));

                LOG_LAST_ERROR_IF(!logFile);
            }

            wsl::windows::common::helpers::LaunchDebugConsole(
                m_comPipe0.c_str(),
                !!m_dmesgCollector,
                m_restrictedToken.get(),
                logFile ? logFile.get() : nullptr,
                !m_vmConfig.EnableTelemetry,
                m_processJobObject.get());
        }
        CATCH_LOG()
    }

    // Create the utility VM through the selected backend. WslCoreVm owns WSL protocol and
    // product policy; the backend owns the platform-specific virtual machine lifetime.
    auto backendRequest = GenerateBackendRequest(VmId, backendKind);
    {
        SlowOperationWatcher slowOperation{"CreateVirtualMachineBackend"};
        m_backend = CreateVirtualMachineBackend(backendKind, backendRequest);
    }
    m_vmCrashEvent.reset(m_backend->GetCrashEvent().release());
    m_runtimeId = m_backend->GetDescription().Identity.VmId;
    WI_ASSERT(IsEqualGUID(VmId, m_runtimeId));

    const auto& description = m_backend->GetDescription();
    if (m_vmConfig.EnableNestedVirtualization && !description.Processor.NestedVirtualization)
    {
        m_vmConfig.EnableNestedVirtualization = false;
        EMIT_USER_WARNING(wsl::shared::Localization::MessageNestedVirtualizationNotSupported());
    }

    const auto systemDistro = description.BootDisks.find(L"system-distro");
    if (systemDistro != description.BootDisks.end())
    {
        m_systemDistroDeviceId = systemDistro->second.GuestAddress.Lun;
    }
    const auto kernelModules = description.BootDisks.find(L"kernel-modules");
    if (kernelModules != description.BootDisks.end())
    {
        m_kernelModulesDeviceId = kernelModules->second.GuestAddress.Lun;
    }

    m_guestListener = m_backend->CreateGuestListener(GuestServicePort{LX_INIT_UTILITY_VM_INIT_PORT});

    if (m_vmConfig.MaxCrashDumpCount >= 0)
    {
        m_crashDumpListener = m_backend->CreateGuestListener(GuestServicePort{LX_INIT_UTILITY_VM_CRASH_DUMP_PORT});
        m_crashDumpCollectionThread = std::thread{&WslCoreVm::CollectCrashDumps, this, m_crashDumpListener.value()};
    }

    // Register before starting so an early exit cannot be missed.
    auto* backend = m_backend.get();
    m_backend->RegisterTerminationCallback([this, backend](GUID) { OnExit(backend->GetTerminationReason()); });
    signalEarlyTermination.release();

    // Start the utility VM.
    try
    {
        SlowOperationWatcher slowOperation{"HcsStartSystem"};
        m_backend->Start();
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();
        auto resetBackend = wil::scope_exit([&] { m_backend.reset(); });
        if ((hr == HRESULT_FROM_WIN32(WSAENOTCONN) || hr == HRESULT_FROM_WIN32(WSAECONNRESET) || hr == HRESULT_FROM_WIN32(WSAETIMEDOUT)) &&
            m_vmCrashEvent.wait(1000))
        {
            const auto crashLogPath = m_backend->GetCrashLogPath();
            if (crashLogPath)
            {
                THROW_HR_WITH_USER_ERROR(
                    WSL_E_VM_CRASHED,
                    wsl::shared::Localization::MessageWSL2Crashed() + L"\r\n" +
                        Localization::MessageWSL2CrashedStackTrace(crashLogPath.value()));
            }

            THROW_HR_WITH_USER_ERROR(WSL_E_VM_CRASHED, wsl::shared::Localization::MessageWSL2Crashed());
        }

        throw;
    }

    // Add GPUs to the utility VM.
    if (m_vmConfig.EnableGpuSupport)
    {
        ExecutionContext context(Context::ConfigureGpu);

        m_backend->AddGpu({});

        // Also add 9p shares for the library directories.
        // N.B. These are not hosted by the out-of-proc drvfs 9p server because the GPU shares
        //      should work even if drvfs is disabled.
        const auto gpuPlan9Device =
            m_backend->CreateFileSystemDevice({VmPlan9HostedDevice{GuestServicePort{LX_INIT_UTILITY_VM_PLAN9_PORT}}});
        auto addShare = [&](PCWSTR name, PCWSTR path) {
            VmFileSystemShareRequest share{};
            share.HostPath = path;
            share.Name = name;
            share.ReadOnly = true;
            share.Options = VmPlan9ShareOptions{.AllowOptions = true};
            m_backend->AddFileSystemShare(gpuPlan9Device.Id, share);
        };

        std::wstring path;
        THROW_IF_FAILED(wil::ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\DriverStore\\FileRepository", path));
        addShare(TEXT(LXSS_GPU_DRIVERS_SHARE), path.c_str());

        // N.B. There are inbox and packaged versions of the Direct 3D libraries. The packaged
        //      versions take presidence by using overlayfs in the guest.
        THROW_IF_FAILED(wil::ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\lxss\\lib", path));

        if (wsl::windows::common::filesystem::FileExists(path.c_str()))
        {
            try
            {
                addShare(TEXT(LXSS_GPU_INBOX_LIB_SHARE), path.c_str());
                m_enableInboxGpuLibs = true;
            }
            CATCH_LOG()
        }

#ifdef WSL_GPU_LIB_PATH

        path = TEXT(WSL_GPU_LIB_PATH);

#else

        path = m_installPath / L"lib";

#endif

        addShare(TEXT(LXSS_GPU_PACKAGED_LIB_SHARE), path.c_str());
    }

    // Accept a connection from mini_init with a receive timeout so the service does not get stuck waiting for a response from the VM.
    {
        SlowOperationWatcher slowOperation{"WaitForMiniInitConnect"};
        m_miniInitChannel =
            wsl::shared::SocketChannel{AcceptConnection(m_vmConfig.KernelBootTimeout), "mini_init", {m_terminatingEvent.get()}};
    }

    // Accept the connection from the Linux guest for notifications.
    m_notifyChannel = AcceptConnection(m_vmConfig.KernelBootTimeout);

    // Receive and parse the guest kernel version
    {
        SlowOperationWatcher slowOperation{"ReadGuestCapabilities"};
        ReadGuestCapabilities();
    }

    // Cache the effective swiotlb configuration. The kernel picks a valid GPA, allocates the pool,
    // and publishes the actual (base, size) via sysfs. Only warn when swiotlb was actually
    // requested via the kernel command line; otherwise the kernel correctly doesn't allocate.
    if (m_hvPciSwiotlbBase != 0 && m_hvPciSwiotlbSize != 0)
    {
        if (m_backend->GetCapabilities().Features.test(static_cast<size_t>(VmFeature::GuestDmaWindow)))
        {
            m_backend->ConfigureGuestDma({m_hvPciSwiotlbBase, m_hvPciSwiotlbSize});
        }
    }
    else if (m_vmConfig.SwiotlbSizeBytes != 0)
    {
        EMIT_USER_WARNING(wsl::shared::Localization::MessageSwiotlbKernelUnsupported());
    }

    // Asynchronously add drvfs devices if supported.
    if (m_vmConfig.EnableHostFileSystemAccess)
    {
        std::promise<bool> initialResult;
        m_drvfsInitialResult = initialResult.get_future();
        auto guestDeviceLock = m_guestDeviceLock.lock_exclusive();
        std::thread([this, guestDeviceLock = std::move(guestDeviceLock), initialResult = std::move(initialResult)]() mutable {
            try
            {
                wsl::windows::common::wslutil::SetThreadDescription(L"InitializeDrvfs");
                initialResult.set_value(InitializeDrvFsLockHeld(m_userToken.get()));
            }
            catch (...)
            {
                try
                {
                    initialResult.set_exception(std::current_exception());
                }
                CATCH_LOG()
            }
        }).detach();
    }

    // Mount the system distro.
    // N.B. If using SCSI, the system distro is added during VM creation.
    switch (m_systemDistroDeviceType)
    {
    case LxMiniInitMountDeviceTypePmem:
        m_systemDistroDeviceId = MountFileAsPersistentMemory(m_vmConfig.SystemDistroPath.c_str(), true);
        break;
    }

    // Attempt to create and mount the swap vhd.
    //
    // N.B. This can fail if the target directory is compressed, encrypted, or if
    //      the user does not have write access.
    ULONG swapLun = ULONG_MAX;
    if ((m_systemDistroDeviceId != ULONG_MAX) && (m_vmConfig.SwapSizeBytes > 0))
    {
        try
        {
            {
                // If no user-specified swap vhd file path was specified, use a
                // path in the temp directory.
                auto runAsUser = wil::impersonate_token(m_userToken.get());
                if (m_vmConfig.SwapFilePath.empty())
                {
                    m_vmConfig.SwapFilePath = m_tempPath / L"swap";
                }

                // Ensure the swap vhd ends with the vhdx file extension.
                if (!wsl::windows::common::string::IsPathComponentEqual(
                        m_vmConfig.SwapFilePath.extension().native(), wsl::windows::common::wslutil::c_vhdxFileExtension))
                {
                    m_vmConfig.SwapFilePath += wsl::windows::common::wslutil::c_vhdxFileExtension;
                }

                // Create the VHD with an additional page for swap overhead.
                m_vmConfig.SwapSizeBytes += PAGE_SIZE;
                auto result = wil::ResultFromException([&]() {
                    wsl::core::filesystem::CreateVhd(m_vmConfig.SwapFilePath.c_str(), m_vmConfig.SwapSizeBytes, &m_userSid.Sid, false, false);
                    m_swapFileCreated = true;
                });

                if (result == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS))
                {
                    auto handle = wsl::core::filesystem::OpenVhd(
                        m_vmConfig.SwapFilePath.c_str(), VIRTUAL_DISK_ACCESS_CREATE | VIRTUAL_DISK_ACCESS_METAOPS | VIRTUAL_DISK_ACCESS_GET_INFO);
                    wsl::core::filesystem::ResizeExistingVhd(handle.get(), m_vmConfig.SwapSizeBytes, RESIZE_VIRTUAL_DISK_FLAG_ALLOW_UNSAFE_VIRTUAL_SIZE);
                }
                else if (FAILED(result))
                {
                    EMIT_USER_WARNING(wsl::shared::Localization::MessagedFailedToCreateSwapVhd(
                        m_vmConfig.SwapFilePath.c_str(), wsl::windows::common::wslutil::GetSystemErrorString(result).c_str()));

                    THROW_HR(result);
                }
            }

            swapLun = AttachDiskLockHeld(m_vmConfig.SwapFilePath.c_str(), DiskType::VHD, MountFlags::None, {}, false, m_userToken.get(), true)
                          .GuestAddress.Lun;
        }
        CATCH_LOG()
    }

    // Validate that the requesting network mode is supported.
    //
    // N.B. This must be done before sending the initial configuration message because some guest
    //      behavior is determined by the networking mode.
    ValidateNetworkingMode();

    // Send the early configuration message.
    wsl::shared::MessageWriter<LX_MINI_INIT_EARLY_CONFIG_MESSAGE> message(LxMiniInitMessageEarlyConfig);
    message->SwapLun = swapLun;
    message->SystemDistroDeviceType = m_systemDistroDeviceType;
    message->SystemDistroDeviceId = m_systemDistroDeviceId;
    message->MemoryReclaimMode = static_cast<LX_MINI_INIT_MEMORY_RECLAIM_MODE>(m_vmConfig.MemoryReclaim);
    message->EnableDebugShell = m_vmConfig.EnableDebugShell;
    message->EnableSafeMode = m_vmConfig.EnableSafeMode;
    // User-mode NAT forwards DNS via the host proxy, so the dedicated DNS hvsocket is only used by HCS NAT and Mirrored modes.
    message->EnableDnsTunneling = m_vmConfig.EnableDnsTunneling && m_vmConfig.NetworkingMode != NetworkingMode::Consomme &&
                                  backendKind != BackendKind::OpenVmm;
    message->DefaultKernel = m_defaultKernel;
    message->IsolateDistroCgroup = m_vmConfig.IsolateDistroCgroup;
    message->KernelModulesDeviceId = m_kernelModulesDeviceId;
    message.WriteString(message->HostnameOffset, wsl::windows::common::filesystem::GetLinuxHostName());
    auto kernelModulesList = m_vmConfig.KernelModulesList;
    if (backendKind == BackendKind::OpenVmm)
    {
        if (!kernelModulesList.empty())
        {
            kernelModulesList += L',';
        }
        kernelModulesList += L"virtio_blk";
    }
    message.WriteString(message->KernelModulesListOffset, kernelModulesList);
    message->DnsTunnelingIpAddress = m_vmConfig.DnsTunnelingIpAddress.value_or(0);

    auto transaction = m_miniInitChannel.StartTransaction();
    transaction.Send<LX_MINI_INIT_EARLY_CONFIG_MESSAGE>(message.Span());

    {
        ExecutionContext context(Context::ConfigureNetworking);

        // Accept the connection from the guest network service and create the channel.
        wsl::core::GnsChannel gnsChannel(AcceptConnection(m_vmConfig.KernelBootTimeout));

        // Create hvsocket connection for DNS tunneling if enabled.
        wil::unique_socket dnsTunnelingSocket;
        if (message->EnableDnsTunneling)
        {
            dnsTunnelingSocket = AcceptConnection(m_vmConfig.KernelBootTimeout);
        }

        // Record the start time of the networking engine initialization so the duration can be logged.
        const auto startTime = std::chrono::steady_clock::now();

        // For NAT networking, ensure the network can be created. If creating the network fails, fall back to
        // Consomme networking mode.
        wsl::windows::common::hcs::unique_hcn_network natNetwork;
        if (backendKind == BackendKind::Hcs && m_vmConfig.NetworkingMode == NetworkingMode::Nat)
        {
            {
                SlowOperationWatcher slowOperation{"CreateNatNetwork"};
                natNetwork = wsl::core::NatNetworking::CreateNetwork(m_vmConfig);
            }
            if (!natNetwork)
            {
                EMIT_USER_WARNING(wsl::shared::Localization::MessageNetworkInitializationFailedFallback2(
                    ToString(m_vmConfig.NetworkingMode), ToString(NetworkingMode::Consomme)));

                m_vmConfig.NetworkingMode = NetworkingMode::Consomme;
            }
        }

        // Create and initialize the networking engine.
        const auto result = wil::ResultFromException(WI_DIAGNOSTICS_INFO, [&] {
            if (backendKind == BackendKind::OpenVmm)
            {
                if (m_vmConfig.NetworkingMode == NetworkingMode::Nat)
                {
                    // N.B. GetDescription() returns by value, so the adapter id must be copied out of the
                    // temporary rather than bound to a reference that would dangle past this statement.
                    const auto adapterId = m_backend->GetDescription().NetworkAdapters.at(L"eth0").Id;
                    m_networkingEngine = std::make_unique<wsl::core::OpenVmmNatNetworking>(
                        *m_backend, adapterId, std::move(gnsChannel), m_vmConfig.EnableLocalhostRelay, m_vmConfig.DhcpTimeout);
                    m_networkingEngine->Initialize();
                }
                else
                {
                    WI_ASSERT(m_vmConfig.NetworkingMode == NetworkingMode::None);
                }

                return;
            }

            // N.B. The existing networking engines still manage HCS resources directly. Keep this
            // concrete escape hatch localized here until they are moved onto IVirtualMachineBackend.
            auto& hcsBackend = static_cast<HcsVirtualMachineBackend&>(*m_backend);
            const auto system = hcsBackend.GetComputeSystemHandle();
            if (m_vmConfig.NetworkingMode == NetworkingMode::Mirrored)
            {
                m_networkingEngine = std::make_unique<wsl::core::MirroredNetworking>(
                    system, std::move(gnsChannel), m_vmConfig, m_runtimeId, std::move(dnsTunnelingSocket));
            }
            else if (m_vmConfig.NetworkingMode == NetworkingMode::Nat)
            {
                WI_ASSERT(natNetwork);

                m_networkingEngine = std::make_unique<wsl::core::NatNetworking>(
                    system, std::move(natNetwork), std::move(gnsChannel), m_vmConfig, std::move(dnsTunnelingSocket));
            }
            else if (m_vmConfig.NetworkingMode == NetworkingMode::Consomme)
            {
                wsl::core::ConsommeNetworkingFlags flags =
                    wsl::core::ConsommeNetworkingFlags::Ipv6 | wsl::core::ConsommeNetworkingFlags::LoopbackClientIp;
                WI_SetFlagIf(flags, wsl::core::ConsommeNetworkingFlags::LocalhostRelay, m_vmConfig.EnableLocalhostRelay);
                WI_SetFlagIf(flags, wsl::core::ConsommeNetworkingFlags::DnsTunneling, m_vmConfig.EnableDnsTunneling);
                // NAT may have fallen back to Consomme after the early-config message; drop the unused DNS hvsocket.
                dnsTunnelingSocket.reset();

                // N.B. Consomme still hosts its virtio-net device through GuestDeviceManager.
                m_networkingEngine = std::make_unique<wsl::core::ConsommeNetworking>(
                    std::move(gnsChannel), flags, LX_INIT_RESOLVCONF_FULL_HEADER, nullptr, hcsBackend.GetGuestDeviceManager(), m_userToken);
            }
            else if (m_vmConfig.NetworkingMode == NetworkingMode::Bridged)
            {
                m_networkingEngine = std::make_unique<wsl::core::BridgedNetworking>(system, m_vmConfig);
            }
            else
            {
                WI_ASSERT(m_vmConfig.NetworkingMode == NetworkingMode::None);
            }

            if (m_networkingEngine)
            {
                m_networkingEngine->Initialize();
            }
        });

        // Find the interface type of the host interface that is most likely to give Internet connectivity
        const auto bestInterfaceIndex = wsl::core::networking::GetBestInterface();
        MIB_IFROW row{};
        row.dwIndex = bestInterfaceIndex;
        IFTYPE bestInterfaceType{};
        // Ignore failures
        if (row.dwIndex != 0 && SUCCEEDED_WIN32(GetIfEntry(&row)))
        {
            bestInterfaceType = row.dwType;
        }

        const auto endTime = std::chrono::steady_clock::now();

        // Log telemetry on the VM initialization including some of its key settings
        WSL_LOG_TELEMETRY(
            "WslCoreVmInitialize",
            PDT_ProductAndServicePerformance,
            TraceLoggingValue(m_runtimeId, "vmId"),
            TraceLoggingValue(ToString(m_vmConfig.NetworkingMode), "networkingMode"),
            TraceLoggingValue(m_vmConfig.FirewallConfig.Enabled(), "firewallEnabled"),
            TraceLoggingValue(m_vmConfig.EnableDnsTunneling, "dnsTunnelingEnabled"),
            TraceLoggingValue(
                m_vmConfig.DnsTunnelingIpAddress.has_value()
                    ? wsl::windows::common::string::IntegerIpv4ToWstring(m_vmConfig.DnsTunnelingIpAddress.value()).c_str()
                    : L"",
                "dnsTunnelingIpAddress"),
            TraceLoggingValue(bestInterfaceType, "bestInterfaceType"),
            TraceLoggingValue(result, "result"),
            TraceLoggingValue((std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime)).count(), "durationMs"));

        if (FAILED(result))
        {
            const auto* context = ExecutionContext::Current();
            if (context != nullptr)
            {
                // We already have a specialized error message, display it to the user.
                const auto& currentError = context->ReportedError();
                if (currentError.has_value())
                {
                    auto strings = wsl::windows::common::wslutil::ErrorToString(currentError.value());
                    EMIT_USER_WARNING(Localization::MessageErrorCode(strings.Message, strings.Code));
                }
            }

            // If something failed during initialization that indicates a dependent service is not running,
            // inform the user to install the Virtual Machine Platform optional component.
            if (wsl::core::networking::IsNetworkErrorForMissingServices(result) &&
                !wsl::windows::common::wslutil::IsVirtualMachinePlatformInstalled())
            {
                wsl::windows::common::notifications::DisplayOptionalComponentsNotification();
                EMIT_USER_WARNING(Localization::MessageVirtualMachinePlatformRequiredForNetworking());
            }

            // Fall back to no networking.
            EMIT_USER_WARNING(wsl::shared::Localization::MessageNetworkInitializationFailedFallback2(
                ToString(m_vmConfig.NetworkingMode), ToString(NetworkingMode::None)));

            m_vmConfig.NetworkingMode = NetworkingMode::None;
            m_networkingEngine.reset();
        }
    }

    // Perform additional initialization.
    InitializeGuest();
}

WslCoreVm::~WslCoreVm() noexcept
{
    TraceLoggingActivity<g_hTraceLoggingProvider, MICROSOFT_KEYWORD_MEASURES> activity;
    TraceLoggingWriteStart(
        activity,
        "TerminateVmStart",
        TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance),
        TraceLoggingValue(m_runtimeId, "vmId"));

    m_networkingEngine.reset();

    auto lock = m_lock.lock_exclusive();

    if (m_drvfsInitialResult.valid())
    {
        try
        {
            m_drvfsInitialResult.get();
        }
        CATCH_LOG()
    }

    // Clear out the exit callback.
    {
        auto exitLock = m_exitCallbackLock.lock_exclusive();
        m_onExit = nullptr;

        // Signal that the vm is terminating
        // N.B. This might have already been signaled if the VM exited abnormally.
        m_terminatingEvent.SetEvent();
    }

    if (m_backend)
    {
        auto closeListener = [&](const VmGuestListener& listener) {
            if (listener.Id.Value != 0)
            {
                LOG_IF_FAILED(wil::ResultFromException([&] { m_backend->CloseGuestListener(listener.Id); }));
            }
        };
        closeListener(m_guestListener);
        if (m_crashDumpListener)
        {
            closeListener(m_crashDumpListener.value());
        }
        if (m_virtioFsListener)
        {
            closeListener(m_virtioFsListener.value());
        }

        bool unexpectedTerminate = m_vmExitEvent.is_signaled();
        bool forcedTerminate = false;

        // Close the socket to mini_init. This will cause mini_init to break out
        // of its message processing loop and perform a clean shutdown.
        m_miniInitChannel.Close();

        if (!unexpectedTerminate)
        {
            // Wait to receive the notification that the VM has exited.
            forcedTerminate = !m_vmExitEvent.wait(UTILITY_VM_SHUTDOWN_TIMEOUT);

            // If the notification did not arrive within the timeout, the VM is
            // forcefully terminated.
            if (forcedTerminate)
            {
                try
                {
                    m_backend->Terminate();
                }
                CATCH_LOG()
            }
        }

        m_vmExitEvent.wait(UTILITY_VM_TERMINATE_TIMEOUT);

        VmTerminationInformation termination;
        {
            auto exitLock = m_exitCallbackLock.lock_shared();
            if (m_terminationInformation)
            {
                termination = m_terminationInformation.value();
            }
        }
        TraceLoggingWriteTagged(
            activity,
            "TerminateVm",
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance),
            TraceLoggingValue(WSL_PACKAGE_VERSION, "wslVersion"),
            TraceLoggingValue(m_runtimeId, "vmId"),
            TraceLoggingValue(forcedTerminate, "forceTerminate"),
            TraceLoggingValue(unexpectedTerminate, "unexpectedTerminate"),
            TraceLoggingValue(m_vmExitEvent.is_signaled(), "terminationCallbackReceived"),
            TraceLoggingValue(termination.Details.c_str(), "exitDetails"));
    }

    // Wait for the distro exit callback thread to exit.
    // The thread might not have been started, in that case joinable() returns false.
    if (m_distroExitThread.joinable())
    {
        m_distroExitThread.join();
    }

    if (m_virtioFsThread.joinable())
    {
        m_virtioFsThread.join();
    }

    if (m_crashDumpCollectionThread.joinable())
    {
        m_crashDumpCollectionThread.join();
    }

    if (m_pluginPlan9Server)
    {
        LOG_IF_FAILED(m_pluginPlan9Server->Teardown());
        m_pluginPlan9Server.reset();
    }

    // Release the backend after WSL-owned device servers have stopped. The backend closes the
    // compute system, guest device manager, disks, and host-side disk state.
    m_backend.reset();

    // Delete the swap vhd if one was created.
    if (m_swapFileCreated)
    {
        try
        {
            const auto runAsUser = wil::impersonate_token(m_userToken.get());
            LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(m_vmConfig.SwapFilePath.c_str()));
        }
        CATCH_LOG()
    }

    // Delete the temp folder if it was created.
    if (m_tempDirectoryCreated)
    {
        try
        {
            const auto runAsUser = wil::impersonate_token(m_userToken.get());
            wil::RemoveDirectoryRecursive(m_tempPath.c_str());
        }
        CATCH_LOG()
    }

    // Delete the mstsc.exe local devices key if one was created.
    if (m_localDevicesKeyCreated)
    {
        try
        {
            const auto runAsUser = wil::impersonate_token(m_userToken.get());
            const auto userKey = wsl::windows::common::registry::OpenCurrentUser();
            const auto key = wsl::windows::common::registry::CreateKey(userKey.get(), c_localDevicesKey, KEY_SET_VALUE);
            THROW_IF_WIN32_ERROR(::RegDeleteKeyValueW(key.get(), nullptr, m_machineId.c_str()));
        }
        CATCH_LOG()
    }

    WSL_LOG("TerminateVmStop");
}

wil::unique_socket WslCoreVm::AcceptConnection(_In_ DWORD ReceiveTimeout, _In_ const std::source_location& Location) const
{
    auto socket = m_guestListener.Accept(m_vmConfig.KernelBootTimeout, Location);

    if (ReceiveTimeout != 0)
    {
        THROW_LAST_ERROR_IF(setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&ReceiveTimeout, sizeof(ReceiveTimeout)) == SOCKET_ERROR);
    }

    return socket;
}

_Requires_lock_held_(m_guestDeviceLock)
void WslCoreVm::AddDrvFsShare(_In_ bool Admin, _In_ HANDLE UserToken)
{
    THROW_HR_IF(HCS_E_TERMINATED, !m_backend || m_backend->GetState() == VmState::Stopped);

    if (m_backend->GetDescription().Backend == BackendKind::Hcs)
    {
        // Allow the Plan 9 server to create NT symlinks.
        //
        // N.B. This may fail for unelevated users, however symlink creation will
        //      succeed even without this privilege if developer mode is enabled.
        wsl::windows::common::security::EnableTokenPrivilege(UserToken, SE_CREATE_SYMBOLIC_LINK_NAME);

        // Set the 9p port and virtio tag.
        const UINT32 port = Admin ? LX_INIT_UTILITY_VM_PLAN9_DRVFS_ADMIN_PORT : LX_INIT_UTILITY_VM_PLAN9_DRVFS_PORT;
        const PCWSTR tag = Admin ? TEXT(LX_INIT_DRVFS_ADMIN_VIRTIO_TAG) : TEXT(LX_INIT_DRVFS_VIRTIO_TAG);
        AddPlan9Share(
            TEXT(LX_INIT_UTILITY_VM_DRVFS_SHARE_NAME), L"\\\\?", port, (hcs::Plan9ShareFlags::AllowOptions | hcs::Plan9ShareFlags::AllowSubPaths), UserToken, tag);
    }
    else
    {
        THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), !m_vmConfig.EnableVirtioFs, "OpenVMM requires virtio-fs");
    }

    const auto virtiofsInitialized = Admin ? m_adminDrvfsToken.is_valid() : m_drvfsToken.is_valid();
    if (m_vmConfig.EnableVirtioFs && !virtiofsInitialized)
    {
        // Add virtiofs devices associating indices with paths from the fixed drive bitmap. These devices support
        // multiple mounts in the guest, so this only needs to be done once.
        auto fixedDrives = wsl::windows::common::filesystem::EnumerateFixedDrives(UserToken).first;
        while (fixedDrives != 0)
        {
            ULONG index;
            WI_VERIFY(_BitScanForward(&index, fixedDrives) != FALSE);
            const wchar_t fixedDrivePath[] = {gsl::narrow_cast<wchar_t>(L'A' + index), L':', L'\\', L'\0'};
            try
            {
                AddVirtioFsShare(Admin, fixedDrivePath, TEXT(LX_INIT_DEFAULT_PLAN9_MOUNT_OPTIONS), UserToken);
            }
            catch (...)
            {
                const auto result = wil::ResultFromCaughtException();
                WSL_LOG(
                    "AddVirtioFsShareError", TraceLoggingValue(fixedDrivePath, "DrivePath"), TraceLoggingValue(result, "result"));
            }
            fixedDrives ^= (1 << index);
        }
    }
}

_Requires_lock_held_(m_guestDeviceLock)
void WslCoreVm::AddPlan9Share(
    _In_ PCWSTR AccessName, _In_ PCWSTR Path, [[maybe_unused]] _In_ UINT32 Port, _In_ hcs::Plan9ShareFlags Flags, _In_ HANDLE UserToken, _In_opt_ PCWSTR VirtIoTag)
{
    const auto serverFactory = [](HANDLE userToken) {
        return wsl::windows::common::wslutil::CreateComServerAsUser<p9fs::Plan9FileSystem, IPlan9FileSystem>(userToken);
    };

    const auto matchingDevice = m_backend->GetFileSystemDevice([&](const VmFileSystemDevice& device) {
        if (m_vmConfig.EnableVirtio9p)
        {
            const auto* virtio = std::get_if<VmPlan9VirtioDevice>(&device.Transport);
            return virtio && virtio->Tag == VirtIoTag;
        }

        const auto* socket = std::get_if<VmPlan9SocketDevice>(&device.Transport);
        return socket && socket->Port.Value == Port;
    });

    VmFileSystemDevice device;
    if (!matchingDevice)
    {
        VmFileSystemDeviceRequest request{};
        request.UserToken = wil::shared_handle{wsl::windows::common::wslutil::DuplicateHandle(UserToken)};
        if (m_vmConfig.EnableVirtio9p)
        {
            request.Transport = VmPlan9VirtioDevice{
                VirtIoTag ? VirtIoTag : L"",
                __uuidof(p9fs::Plan9FileSystem),
                VIRTIO_PLAN9_DEVICE_ID,
                serverFactory,
            };
        }
        else
        {
            request.Transport = VmPlan9SocketDevice{GuestServicePort{Port}, serverFactory};
        }

        device = m_backend->CreateFileSystemDevice(request);
    }
    else
    {
        device = matchingDevice.value();
    }

    VmFileSystemShareRequest request{};
    request.HostPath = Path;
    request.Name = AccessName;
    request.ReadOnly = WI_IsFlagSet(Flags, hcs::Plan9ShareFlags::ReadOnly);
    request.Options = VmPlan9ShareOptions{
        WI_IsFlagSet(Flags, hcs::Plan9ShareFlags::LinuxMetadata),
        WI_IsFlagSet(Flags, hcs::Plan9ShareFlags::CaseSensitive),
        WI_IsFlagSet(Flags, hcs::Plan9ShareFlags::UseShareRootIdentity),
        WI_IsFlagSet(Flags, hcs::Plan9ShareFlags::AllowOptions),
        WI_IsFlagSet(Flags, hcs::Plan9ShareFlags::AllowSubPaths),
    };
    request.UserToken = wil::shared_handle{wsl::windows::common::wslutil::DuplicateHandle(UserToken)};
    m_backend->AddFileSystemShare(device.Id, request);
}

ULONG WslCoreVm::AttachDisk(_In_ PCWSTR Disk, _In_ DiskType Type, _In_ std::optional<ULONG> Lun, _In_ bool IsUserDisk, _In_ HANDLE UserToken)
{
    auto lock = m_lock.lock_exclusive();
    return AttachDiskLockHeld(Disk, Type, MountFlags::None, Lun, IsUserDisk, UserToken).GuestAddress.Lun;
}

VmDiskAttachment WslCoreVm::AttachDiskLockHeld(
    _In_ PCWSTR Disk, _In_ DiskType Type, _In_ MountFlags Flags, _In_ std::optional<ULONG> Lun, _In_ bool IsUserDisk, _In_opt_ HANDLE UserToken, _In_ bool BootCritical)
{
    ExecutionContext context(Context::MountDisk);

    try
    {
        VmDiskRequest request{};
        if (Type == DiskType::PassThrough)
        {
            request.Source = VmPhysicalDiskSource{Disk};
        }
        else
        {
            const std::filesystem::path path{Disk};
            VmDiskFormat format;
            if (wsl::windows::common::string::IsPathComponentEqual(path.extension().native(), wsl::windows::common::wslutil::c_vhdFileExtension))
            {
                format = VmDiskFormat::Vhd;
            }
            else
            {
                THROW_HR_IF(
                    E_INVALIDARG,
                    !wsl::windows::common::string::IsPathComponentEqual(path.extension().native(), wsl::windows::common::wslutil::c_vhdxFileExtension));
                format = VmDiskFormat::Vhdx;
            }

            request.Source = VmVirtualDiskSource{path, format};
        }
        request.ReadOnly = WI_IsFlagSet(Flags, MountFlags::ReadOnly);
        request.UserDisk = IsUserDisk;
        request.BootCritical = BootCritical;
        request.DeviceTimeout = std::chrono::milliseconds{m_vmConfig.MountDeviceTimeout};
        if (Lun)
        {
            request.Placement = VmScsiPlacement{{0, Lun.value()}};
        }
        if (UserToken)
        {
            request.UserToken = wil::shared_handle{wsl::windows::common::wslutil::DuplicateHandle(UserToken)};
        }

        const auto attachment = m_backend->AttachDisk(request);
        const auto attachedDisks = m_backend->GetAttachedDisks();
        std::erase_if(m_diskMounts, [&](const auto& entry) {
            return std::ranges::none_of(attachedDisks, [&](const VmDiskAttachment& disk) { return disk.Id.Value == entry.first; });
        });
        m_diskMounts.try_emplace(attachment.Id.Value);
        return attachment;
    }
    catch (...)
    {
        const auto result = wil::ResultFromCaughtException();
        THROW_HR_WITH_USER_ERROR(
            result, Localization::MessageFailedToAttachDisk(Disk, wsl::windows::common::wslutil::GetSystemErrorString(result)));
    }
}

void WslCoreVm::CollectCrashDumps(VmGuestListener Listener) const
{
    wsl::windows::common::wslutil::SetThreadDescription(L"CrashDumpCollection");

    while (!m_terminatingEvent.is_signaled())
    {
        try
        {
            auto socket = Listener.Accept();

            DWORD receiveTimeout = m_vmConfig.KernelBootTimeout;
            THROW_LAST_ERROR_IF(setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&receiveTimeout, sizeof(receiveTimeout)) == SOCKET_ERROR);

            auto channel = wsl::shared::SocketChannel{std::move(socket), "crash_dump", {m_terminatingEvent.get()}};

            auto transaction = channel.ReceiveTransaction();
            gsl::span<gsl::byte> responseSpan;
            const auto& message = transaction.Receive<LX_PROCESS_CRASH>(&responseSpan);

            // Safely extract the process name from the flexible array member.
            // The buffer may not be NUL-terminated, so bound the length to the received span size.
            const auto bufferSize = responseSpan.size_bytes() - offsetof(LX_PROCESS_CRASH, Buffer);
            const std::string process(message.Buffer, strnlen(message.Buffer, bufferSize));

            constexpr auto dumpExtension = ".dmp";
            constexpr auto dumpPrefix = "wsl-crash";

            auto filename = std::format("{}-{}-{}-{}-{}{}", dumpPrefix, message.Timestamp, message.Pid, process, message.Signal, dumpExtension);

            std::replace_if(
                filename.begin(),
                filename.end(),
                [](char e) { return !std::isalnum(static_cast<unsigned char>(e)) && e != '.' && e != '-'; },
                '_');

            auto fullPath = m_vmConfig.CrashDumpFolder / filename;

            // Log telemetry when there is a crash within the WSL VM
            WSL_LOG_TELEMETRY(
                "LinuxCrash",
                PDT_ProductAndServicePerformance,
                TraceLoggingValue(fullPath.c_str(), "FullPath"),
                TraceLoggingValue(message.Pid, "Pid"),
                TraceLoggingValue(message.Signal, "Signal"),
                TraceLoggingValue(process.c_str(), "process"));

            auto runAsUser = wil::impersonate_token(m_userToken.get());

            std::error_code error;
            std::filesystem::create_directories(m_vmConfig.CrashDumpFolder, error);
            if (error.value())
            {
                THROW_WIN32_MSG(error.value(), "Failed to create folder: %ls", m_vmConfig.CrashDumpFolder.c_str());
            }

            // Only delete files that:
            // - have the temporary flag set
            // - start with 'wsl-crash'
            // - end in .dmp
            //
            // This logic is here to prevent accidental user file deletion

            auto pred = [&dumpExtension, &dumpPrefix](const auto& e) {
                return WI_IsFlagSet(GetFileAttributes(e.path().c_str()), FILE_ATTRIBUTE_TEMPORARY) && e.path().has_extension() &&
                       e.path().extension() == dumpExtension && e.path().has_filename() &&
                       e.path().filename().string().find(dumpPrefix) == 0;
            };

            wsl::windows::common::wslutil::EnforceFileLimit(m_vmConfig.CrashDumpFolder.c_str(), m_vmConfig.MaxCrashDumpCount, pred);

            wil::unique_hfile file{CreateFileW(fullPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr)};
            THROW_LAST_ERROR_IF(!file);

            transaction.SendResultMessage<std::int32_t>(0);

            wsl::windows::common::relay::InterruptableRelay(reinterpret_cast<HANDLE>(channel.Socket()), file.get(), nullptr);
        }
        CATCH_LOG();
    }
}

std::shared_ptr<LxssRunningInstance> WslCoreVm::CreateInstance(
    _In_ const GUID& InstanceId,
    _In_ const LXSS_DISTRO_CONFIGURATION& Configuration,
    _In_ LX_MESSAGE_TYPE MessageType,
    _In_ DWORD ReceiveTimeout,
    _In_ ULONG DefaultUid,
    _In_ ULONG64 ClientLifetimeId,
    _In_ ULONG ExportFlags,
    _Out_opt_ ULONG* ConnectPort)
{
    // Add the VHD to the machine.
    auto lock = m_lock.lock_exclusive();
    SlowOperationWatcher slowOperation{"AttachDistroVhd"};
    const auto attachment =
        AttachDiskLockHeld(Configuration.VhdFilePath.c_str(), DiskType::VHD, MountFlags::None, {}, false, m_userToken.get());
    slowOperation.Reset();

    // Launch the init daemon and create the instance.
    int flags = LxMiniInitMessageFlagNone;
    std::wstring sharedMemoryRoot{};

#ifdef WSL_DEV_INSTALL_PATH

    std::wstring installPath = TEXT(WSL_DEV_INSTALL_PATH);

#else

    std::wstring installPath = m_installPath.wstring();

#endif

    std::wstring userProfile{};
    if (LXSS_ENABLE_GUI_APPS() && (MessageType == LxMiniInitMessageLaunchInit))
    {
        WI_SetFlag(flags, LxMiniInitMessageFlagLaunchSystemDistro);
        sharedMemoryRoot = m_sharedMemoryRoot;

        userProfile = m_userProfile;
    }

    WI_SetFlagIf(flags, LxMiniInitMessageFlagExportCompressGzip, WI_IsFlagSet(ExportFlags, LXSS_EXPORT_DISTRO_FLAGS_GZIP));
    WI_SetFlagIf(flags, LxMiniInitMessageFlagExportCompressXzip, WI_IsFlagSet(ExportFlags, LXSS_EXPORT_DISTRO_FLAGS_XZIP));
    WI_SetFlagIf(flags, LxMiniInitMessageFlagVerbose, WI_IsFlagSet(ExportFlags, LXSS_EXPORT_DISTRO_FLAGS_VERBOSE));

    wsl::shared::MessageWriter<LX_MINI_INIT_MESSAGE> message(MessageType);
    message->MountDeviceType = ToMiniInitDeviceType(attachment.Transport);
    message->DeviceId = attachment.GuestAddress.Lun;
    message->Flags = flags;
    message.WriteString(message->FsTypeOffset, "ext4");
    message.WriteString(message->MountOptionsOffset, "discard,errors=remount-ro,data=ordered");
    message.WriteString(message->VmIdOffset, m_machineId);
    message.WriteString(message->DistributionNameOffset, Configuration.Name);
    message.WriteString(message->SharedMemoryRootOffset, sharedMemoryRoot);
    message.WriteString(message->InstallPathOffset, installPath);
    message.WriteString(message->UserProfileOffset, userProfile);
    auto transaction = m_miniInitChannel.StartTransaction();
    transaction.Send<LX_MINI_INIT_MESSAGE>(message.Span());

    return CreateInstanceInternal(
        InstanceId, Configuration, ReceiveTimeout, DefaultUid, ClientLifetimeId, WI_IsFlagSet(flags, LxMiniInitMessageFlagLaunchSystemDistro), ConnectPort);
}

std::shared_ptr<LxssRunningInstance> WslCoreVm::CreateInstanceInternal(
    _In_ const GUID& InstanceId,
    _In_ const LXSS_DISTRO_CONFIGURATION& Configuration,
    _In_ DWORD ReceiveTimeout,
    _In_ ULONG DefaultUid,
    _In_ ULONG64 ClientLifetimeId,
    _In_ bool LaunchSystemDistro,
    _Out_opt_ ULONG* ConnectPort)
{
    // Clear the drive mounting flag if support is disabled at the VM level.
    //
    // N.B. If the system distro is enabled the share will still be created since
    //      GUI apps require access to the Windows file system in order to launch mstsc.
    LXSS_DISTRO_CONFIGURATION localConfig = Configuration;
    WI_ClearFlagIf(localConfig.Flags, LXSS_DISTRO_FLAGS_ENABLE_DRIVE_MOUNTING, !m_vmConfig.EnableHostFileSystemAccess);

    // Establish a communication channel with the init daemon.
    SlowOperationWatcher slowOperation{"WaitForInitDaemonConnect"};
    auto initSocket = AcceptConnection(ReceiveTimeout);
    slowOperation.Reset();

    // If the system distro is enabled, establish a communication channel with its init daemon.
    wil::unique_socket systemDistroSocket;
    if (LaunchSystemDistro)
    {
        WI_ASSERT(m_vmConfig.EnableGuiApps);
        systemDistroSocket = AcceptConnection(ReceiveTimeout);
    }

    // Set feature flags for the instance.
    ULONG featureFlags{};
    WI_SetFlagIf(featureFlags, LxInitFeatureVirtIo9p, m_vmConfig.EnableVirtio9p);
    WI_SetFlagIf(featureFlags, LxInitFeatureVirtIoFs, m_vmConfig.EnableVirtioFs);
    WI_SetFlagIf(featureFlags, LxInitFeatureDnsTunneling, m_vmConfig.EnableDnsTunneling);

    // Create an instance, this takes ownership of the sockets.
    auto instance = std::make_shared<WslCoreInstance>(
        m_userToken.get(),
        initSocket,
        systemDistroSocket,
        InstanceId,
        m_runtimeId,
        localConfig,
        DefaultUid,
        ClientLifetimeId,
        m_initializeDrvFs,
        [this](ULONG Port, HANDLE ExitHandle) { return m_backend->ConnectGuest(GuestServicePort{Port}, ExitHandle); },
        m_backend->GetGuestConnector(),
        featureFlags,
        m_vmConfig.DistributionStartTimeout,
        m_vmConfig.InstanceIdleTimeout,
        ConnectPort,
        m_processJobObject.get());

    WI_ASSERT(!initSocket && !systemDistroSocket);

    return instance;
}

std::pair<int, LX_MINI_MOUNT_STEP> WslCoreVm::DetachDisk(_In_opt_ PCWSTR Disk)
{
    bool deleted = !ARGUMENT_PRESENT(Disk);

    auto diskMatches = [TargetPath = Disk](const VmDiskAttachment& disk) {
        if (!disk.UserDisk)
        {
            // Only user mounted disks can be detached.
            return false;
        }

        if (!disk.PassThrough)
        {
            std::error_code error{};
            return TargetPath == nullptr || std::filesystem::equivalent(disk.Path, TargetPath, error);
        }

        return TargetPath == nullptr || wsl::windows::common::string::IsPathComponentEqual(disk.Path, TargetPath);
    };

    auto lock = m_lock.lock_exclusive();
    for (const auto& disk : m_backend->GetAttachedDisks())
    {
        if (!diskMatches(disk))
        {
            continue;
        }

        if (const auto mountState = m_diskMounts.find(disk.Id.Value); mountState != m_diskMounts.end())
        {
            const auto result = UnmountDisk(disk, mountState->second);
            if (result.first != 0)
            {
                return result;
            }
        }

        m_backend->DetachDisk(disk.Id);
        m_diskMounts.erase(disk.Id.Value);
        deleted = true;
    }

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !deleted);

    return std::make_pair(0, LxMiniInitMountStepNone);
}

void WslCoreVm::EjectVhd(_In_ PCWSTR VhdPath)
{
    auto lock = m_lock.lock_exclusive();
    return EjectVhdLockHeld(VhdPath);
}

_Requires_lock_held_(m_lock)
void WslCoreVm::EjectVhdLockHeld(_In_ PCWSTR VhdPath)
{
    const auto disks = m_backend->GetAttachedDisks();
    const auto disk = std::find_if(disks.begin(), disks.end(), [VhdPath](const VmDiskAttachment& entry) {
        return !entry.PassThrough && wsl::windows::common::string::IsPathComponentEqual(entry.Path, VhdPath);
    });
    if (disk != disks.end())
    {
        EJECT_VHD_MESSAGE message;
        message.Header.MessageSize = sizeof(message);
        message.Header.MessageType = LxMiniInitMessageEjectVhd;
        message.DeviceType = ToMiniInitDeviceType(disk->Transport);
        message.DeviceId = disk->GuestAddress.Lun;
        const auto& result = m_miniInitChannel.Transaction(message);
        LOG_HR_IF_MSG(E_UNEXPECTED, result.Result != 0, "VHD eject failed: %u", result.Result);

        m_backend->DetachDisk(disk->Id);
        m_diskMounts.erase(disk->Id.Value);
    }
}

_Requires_lock_held_(m_guestDeviceLock)
std::optional<VmFileSystemShare> WslCoreVm::FindVirtioFsShare(_In_ PCWSTR Tag, _In_ std::optional<bool> Admin) const
{
    return m_backend->GetFileSystemShare([&](const VmFileSystemShare& share) {
        const auto* address = std::get_if<VmVirtioFsShareAddress>(&share.GuestAddress);
        if (!address)
        {
            return false;
        }

        const auto& name = address->ChildName ? address->ChildName.value() : address->Tag;
        return name == Tag && (!Admin.has_value() || share.Elevated == Admin.value());
    });
}

void WslCoreVm::ValidateBackendConfiguration(BackendKind Backend) const
{
    if (Backend == BackendKind::Hcs)
    {
        return;
    }

    constexpr auto notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    THROW_HR_IF_MSG(notSupported, wsl::shared::Arm64, "OpenVMM is supported only on x64");
    THROW_HR_IF_MSG(notSupported, m_vmConfig.EnableGpuSupport, "OpenVMM does not support GPU assignment");
    THROW_HR_IF_MSG(notSupported, m_vmConfig.EnableGuiApps, "OpenVMM does not support GUI applications");
    THROW_HR_IF_MSG(notSupported, m_vmConfig.EnableVirtio9p, "OpenVMM does not support virtio-9p file sharing");
    THROW_HR_IF_MSG(
        notSupported,
        m_vmConfig.EnableHostFileSystemAccess && !m_vmConfig.EnableVirtioFs,
        "OpenVMM host filesystem access requires virtio-fs");
    THROW_HR_IF_MSG(
        notSupported,
        m_vmConfig.NetworkingMode != NetworkingMode::None && m_vmConfig.NetworkingMode != NetworkingMode::Nat,
        "OpenVMM only supports NAT or disabled networking");
}

VmCreateRequest WslCoreVm::GenerateBackendRequest(const GUID& VmId, BackendKind Backend)
{
    VmCreateRequest request{};
    request.Identity = {VmId, m_userToken};
    request.Owner = wsl::windows::common::wslutil::c_vmOwner;
    request.EnableTelemetry = m_vmConfig.EnableTelemetry;
    request.Processor.Count = m_vmConfig.ProcessorCount;
    request.Processor.NestedVirtualization = m_vmConfig.EnableNestedVirtualization ? VmFeatureRequest::Preferred : VmFeatureRequest::Disabled;
    request.Processor.PerfmonPmu = m_vmConfig.EnableHardwarePerformanceCounters ? VmFeatureRequest::Preferred : VmFeatureRequest::Disabled;
    request.Processor.PerfmonLbr = request.Processor.PerfmonPmu;

    request.Memory.SizeBytes = (m_vmConfig.MemorySizeBytes / (2 * _1MB)) * (2 * _1MB);
    const auto backendCapabilities = QueryVirtualMachineBackendCapabilities(Backend);
    auto requestFeature = [&](VmFeature Feature) {
        return backendCapabilities.Features.test(static_cast<size_t>(Feature)) ? VmFeatureRequest::Required : VmFeatureRequest::Disabled;
    };
    request.Memory.AllowOvercommit = requestFeature(VmFeature::MemoryOvercommit);
    request.Memory.DeferredCommit = requestFeature(VmFeature::DeferredMemoryCommit);
    request.Memory.ColdDiscard = requestFeature(VmFeature::ColdDiscard);
    if (backendCapabilities.Features.test(static_cast<size_t>(VmFeature::SmallPageMemory)))
    {
        request.Memory.SmallPageBacking = VmFeatureRequest::Required;
        request.Memory.FaultClusterSizeShift = 4;
        request.Memory.DirectMapFaultClusterSizeShift = 4;
        request.Memory.PageReportingOrder = 5;
        m_pageReportingOrder = 5;
    }
    else
    {
        m_pageReportingOrder = 9;
    }

    if (backendCapabilities.Features.test(static_cast<size_t>(VmFeature::HostingProcessNameSuffix)))
    {
        request.Memory.HostingProcessNameSuffix = wsl::windows::common::wslutil::c_vmOwner;
    }

    bool privateSystemDistro = !m_vmConfig.SystemDistroPath.empty();
    if (!privateSystemDistro)
    {
#ifdef WSL_SYSTEM_DISTRO_PATH
        m_vmConfig.SystemDistroPath = TEXT(WSL_SYSTEM_DISTRO_PATH);
        privateSystemDistro = true;
#else
        m_systemDistroDeviceType = LxMiniInitMountDeviceTypeScsi;
        m_vmConfig.SystemDistroPath = m_installPath / L"system.vhd";
        WI_ASSERT(wsl::windows::common::filesystem::FileExists(m_vmConfig.SystemDistroPath.c_str()));
#endif
    }

    if (privateSystemDistro)
    {
        if (wsl::windows::common::string::IsPathComponentEqual(m_vmConfig.SystemDistroPath.extension().native(), L".img"))
        {
            m_systemDistroDeviceType = LxMiniInitMountDeviceTypePmem;
        }
        else if (wsl::windows::common::string::IsPathComponentEqual(m_vmConfig.SystemDistroPath.extension().native(), L".vhd"))
        {
            m_systemDistroDeviceType = LxMiniInitMountDeviceTypeScsi;
        }

        THROW_HR_IF(
            WSL_E_CUSTOM_SYSTEM_DISTRO_ERROR,
            m_systemDistroDeviceType == LxMiniInitMountDeviceTypeInvalid ||
                !wsl::windows::common::filesystem::FileExists(m_vmConfig.SystemDistroPath.c_str()));
    }

    SafeInt<INT64> highMmioGapInMB = 0;
    if (backendCapabilities.Features.test(static_cast<size_t>(VmFeature::HighMmio)))
    {
        highMmioGapInMB = DEFAULT_HIGH_MMIO_GAP_IN_MB;
        if (m_vmConfig.EnableGuiApps && m_vmConfig.EnableVirtio)
        {
            highMmioGapInMB += WSLG_SHARED_MEMORY_SIZE_MB + EXTRA_MMIO_SIZE_PER_VIRTIOFS_DEVICE_IN_MB;
        }
        if (m_systemDistroDeviceType == LxMiniInitMountDeviceTypePmem)
        {
            highMmioGapInMB += RequiredExtraMmioSpaceForPmemFileInMb(m_vmConfig.SystemDistroPath.c_str());
        }
        request.Mmio.HighWindowSizeBytes = static_cast<std::uint64_t>(highMmioGapInMB) * _1MB;
        request.Mmio.MaximumGuestAddressBits = 36;
    }

    WSL_LOG(
        "InitializeSystemDistro",
        TraceLoggingValue(static_cast<INT64>(highMmioGapInMB), "highMmioGapInMB"),
        TraceLoggingValue(privateSystemDistro, "privateSystemDistro"),
        TraceLoggingValue(static_cast<DWORD>(m_systemDistroDeviceType), "systemDistroDeviceType"),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO));

    std::wstring kernelCmdLine = L"initrd=\\" LXSS_VM_MODE_INITRD_NAME L" " TEXT(WSL_ROOT_INIT_ENV) L"=1 panic=-1";
    helpers::AppendCommonKernelCommandLine(kernelCmdLine, m_pageReportingOrder, m_vmConfig.SwiotlbSizeBytes, m_vmConfig.ProcessorCount);

    if (m_dmesgCollector)
    {
        if (m_vmConfig.EnableEarlyBootLogging)
        {
            kernelCmdLine += wsl::shared::Arm64 ? L" earlycon=pl011,0xeffec000,115200" : L" earlycon=uart8250,io,0x3f8,115200";
            request.Consoles.push_back({VmConsoleRole::EarlyBoot, VmSerialConsole{0, m_dmesgCollector->EarlyConsoleName()}});
        }
        kernelCmdLine += L" console=hvc0 debug";
        request.Consoles.push_back({VmConsoleRole::KernelConsole, VmVirtioConsole{0, L"hvc0", m_dmesgCollector->VirtioConsoleName()}});
    }
    else if (m_vmConfig.EnableDebugConsole)
    {
        kernelCmdLine += wsl::shared::Arm64 ? L" console=ttyAMA0 debug" : L" console=ttyS0,115200 debug";
    }

    if (m_gnsTelemetryLogger)
    {
        request.Consoles.push_back(
            {VmConsoleRole::Telemetry, VmVirtioConsole{1, TEXT(LX_INIT_HVC_TELEMETRY), m_gnsTelemetryLogger->GetPipeName()}});
    }
    if (!m_debugShellPipe.empty())
    {
        request.Consoles.push_back({VmConsoleRole::DebugShell, VmVirtioConsole{2, TEXT(LX_INIT_HVC_DEBUG_SHELL), m_debugShellPipe}});
    }

    if (m_vmConfig.KernelDebugPort != 0)
    {
        const auto debugDeviceName = wsl::shared::Arm64 ? L"ttyAMA1" : L"ttyS1";
        kernelCmdLine += std::format(L" pty.legacy_count=2 kgdboc={},115200", debugDeviceName);
        m_comPipe1 = wsl::windows::common::helpers::GetUniquePipeName();
        request.Consoles.push_back({VmConsoleRole::KernelDebugger, VmSerialConsole{1, m_comPipe1}});
        wsl::windows::common::helpers::LaunchKdRelay(
            m_comPipe1.c_str(),
            m_restrictedToken.get(),
            m_vmConfig.KernelDebugPort,
            m_terminatingEvent.get(),
            !m_vmConfig.EnableTelemetry,
            m_processJobObject.get());
    }
    else
    {
        kernelCmdLine += L" pty.legacy_count=0";
    }

    if (Backend != BackendKind::OpenVmm && !m_comPipe0.empty() && (!m_dmesgCollector || !m_vmConfig.EnableEarlyBootLogging))
    {
        request.Consoles.push_back({VmConsoleRole::KernelConsole, VmSerialConsole{0, m_comPipe0}});
    }
    if (m_vmConfig.MaxCrashDumpCount >= 0)
    {
        kernelCmdLine += L" " WSL_ENABLE_CRASH_DUMP_ENV L"=1";
        VmCrashCaptureRequest crashCapture{m_vmConfig.CrashDumpFolder, gsl::narrow_cast<std::uint32_t>(m_vmConfig.MaxCrashDumpCount)};

        // HCS pre-creates an empty saved-state file that lives in the folder for the VM's lifetime, so only OpenVMM,
        // which writes its dump when the guest crashes, captures saved state into the user's crash dump folder.
        if (Backend == BackendKind::OpenVmm)
        {
            crashCapture.SavedStateFolder = m_vmConfig.CrashDumpFolder;
        }

        crashCapture.Policy = VmSelectionPolicy::Preferred;
        request.CrashCapture = std::move(crashCapture);
    }
    if (!m_vmConfig.KernelCommandLine.empty())
    {
        kernelCmdLine += L" ";
        kernelCmdLine += m_vmConfig.KernelCommandLine;
    }

    request.Boot.Method = wsl::shared::Arm64 ? VmBootMethod::Uefi : VmBootMethod::LinuxDirect;
    request.Boot.KernelPath = wsl::shared::Arm64 ? m_rootFsPath / LXSS_VM_MODE_KERNEL_NAME : m_vmConfig.KernelPath;
    if constexpr (!wsl::shared::Arm64)
    {
        request.Boot.InitrdPath = m_rootFsPath / LXSS_VM_MODE_INITRD_NAME;
    }
    request.Boot.KernelCommandLine = std::move(kernelCmdLine);

    auto addBootDisk = [&](std::wstring key, const std::filesystem::path& path, bool grantHostAccess) {
        VmBootDiskRequest disk{};
        disk.Key = std::move(key);
        disk.Disk.Source = VmVirtualDiskSource{path, VmDiskFormat::Vhd};
        disk.Disk.ReadOnly = true;
        disk.Disk.DeviceTimeout = std::chrono::milliseconds{m_vmConfig.MountDeviceTimeout};
        disk.GrantHostAccess = grantHostAccess;
        request.BootDisks.emplace_back(std::move(disk));
    };
    if (m_systemDistroDeviceType == LxMiniInitMountDeviceTypeScsi)
    {
        addBootDisk(L"system-distro", m_vmConfig.SystemDistroPath, privateSystemDistro);
    }
    if (!m_vmConfig.KernelModulesPath.empty())
    {
        addBootDisk(L"kernel-modules", m_vmConfig.KernelModulesPath, m_privateKernelModules);
    }
    if (Backend == BackendKind::OpenVmm && m_vmConfig.NetworkingMode == NetworkingMode::Nat)
    {
        request.NetworkAdapters.emplace_back(wsl::core::CreateOpenVmmNatNetworkAdapterRequest(m_vmConfig.EnableDnsTunneling));
    }

    return request;
}

std::pair<int, LX_MINI_MOUNT_STEP> WslCoreVm::GetMountResult(_In_ wsl::shared::SocketChannel& Channel)
{
    // Read the response from mini_init.
    const auto& Message = Channel.ReceiveMessage<LX_MINI_INIT_MOUNT_RESULT_MESSAGE>();
    return std::make_pair(Message.Result, Message.FailureStep);
}

const wsl::core::Config& WslCoreVm::GetConfig() const noexcept
{
    return m_vmConfig;
}

GUID WslCoreVm::GetRuntimeId() const
{
    return m_runtimeId;
}

int WslCoreVm::GetVmIdleTimeout() const
{
    return m_vmConfig.VmIdleTimeout;
}

void WslCoreVm::InitializeGuest()
{
    // If GUI apps are enabled, mount the shared memory device and write a registry key to suppress mstsc.exe security warnings.
    if (LXSS_ENABLE_GUI_APPS())
    {
        if (m_vmConfig.EnableVirtio)
        {
            try
            {
                const auto sharedMemory = m_backend->AddSharedMemory({L"wslg", L"wslg", WSLG_SHARED_MEMORY_SIZE_MB * _1MB, m_userToken});
                m_sharedMemoryRoot = sharedMemory.ObjectPath;
            }
            CATCH_LOG()
        }

        try
        {
            auto runAsUser = wil::impersonate_token(m_userToken.get());
            const auto userKey = wsl::windows::common::registry::OpenCurrentUser();
            const auto devicesKey = wsl::windows::common::registry::CreateKey(userKey.get(), c_localDevicesKey, KEY_SET_VALUE);
            constexpr DWORD flags = 0xC4; // Allow clipboard, microphone, and printer access.
            wsl::windows::common::registry::WriteDword(devicesKey.get(), nullptr, m_machineId.c_str(), flags);
            m_localDevicesKeyCreated = true;
        }
        CATCH_LOG()
    }

    // Calculate the size of the configuration message.
    wsl::shared::MessageWriter<LX_MINI_INIT_CONFIG_MESSAGE> message(LxMiniInitMessageInitialConfig);
    message->EntropySize = c_bootEntropy;
    message->EnableGuiApps = LXSS_ENABLE_GUI_APPS();
    message->MountGpuShares = m_vmConfig.EnableGpuSupport;
    message->EnableInboxGpuLibs = m_enableInboxGpuLibs;
    if (m_networkingEngine)
    {
        m_networkingEngine->FillInitialConfiguration(message->NetworkingConfiguration);
    }

    WI_ASSERT(message->NetworkingConfiguration.NetworkingMode == static_cast<LX_MINI_INIT_NETWORKING_MODE>(m_vmConfig.NetworkingMode));

    // Generate additional entropy to be injected.
    if (message->EntropySize > 0)
    {
        THROW_IF_NTSTATUS_FAILED(BCryptGenRandom(
            nullptr, (PUCHAR)message.InsertBuffer(message->EntropyOffset, message->EntropySize).data(), message->EntropySize, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    }

    // Send the message.
    auto transaction = m_miniInitChannel.StartTransaction();
    transaction.Send<LX_MINI_INIT_CONFIG_MESSAGE>(message.Span());

    // If port tracker or localhost relay are enabled, establish a connection with the guest and start processing messages.
    switch (message->NetworkingConfiguration.PortTrackerType)
    {
    case LxMiniInitPortTrackerTypeMirrored:
    {
        auto socket = AcceptConnection(m_vmConfig.KernelBootTimeout);
        m_networkingEngine->StartPortTracker(std::move(socket));
        break;
    }
    case LxMiniInitPortTrackerTypeRelay:
    {
        // If localhost relay is enabled, create a relay process.
        //
        // N.B. The relay process is launched at medium integrity level, and its lifetime is tied to the lifetime of the utility VM.
        const auto result = wil::ResultFromException(WI_DIAGNOSTICS_INFO, [&]() {
            const auto socket = AcceptConnection(m_vmConfig.KernelBootTimeout);
            wsl::windows::common::helpers::LaunchPortRelay(
                socket.get(), m_runtimeId, m_restrictedToken.get(), !m_vmConfig.EnableTelemetry, m_processJobObject.get());
        });

        if (FAILED(result))
        {
            const auto errorString = wsl::windows::common::wslutil::GetSystemErrorString(result);
            EMIT_USER_WARNING(wsl::shared::Localization::MessageLocalhostRelayFailed(errorString));
        }
        break;
    }

    default:
        break;
    }
}

// Returns true if the admin drvfs share should be used,
// false if the non-elevated share should be used
bool WslCoreVm::InitializeDrvFs(_In_ HANDLE UserToken)
{
    auto guestDeviceLock = m_guestDeviceLock.lock_exclusive();
    WI_ASSERT(m_vmConfig.EnableHostFileSystemAccess);
    if (m_drvfsInitialResult.valid())
    {
        // The drvfs drives might have been initialized with a different token.
        // Make sure the elevation status matches before returning the cached value.
        const auto elevated = wsl::windows::common::security::IsTokenElevated(UserToken);
        if (m_drvfsInitialResult.get() == elevated)
        {
            return elevated;
        }
    }

    return InitializeDrvFsLockHeld(UserToken);
}

// Returns true if the admin drvfs share should be used,
// false if the non-elevated share should be used
_Requires_lock_held_(m_guestDeviceLock)
bool WslCoreVm::InitializeDrvFsLockHeld(_In_ HANDLE UserToken)
{
    // Before checking whether DrvFs is already initialized, make sure any existing Plan 9 servers
    // are usable.
    VerifyPlan9Servers();

    const auto elevated = wsl::windows::common::security::IsTokenElevated(UserToken);
    if (m_backend->GetDescription().Backend == BackendKind::OpenVmm)
    {
        const auto tokenUser = wil::get_token_information<TOKEN_USER>(UserToken);
        THROW_HR_IF(E_ACCESSDENIED, !EqualSid(&m_userSid.Sid, tokenUser->User.Sid));
        THROW_HR_IF_MSG(
            E_ACCESSDENIED,
            elevated != wsl::windows::common::security::IsTokenElevated(m_userToken.get()),
            "OpenVMM DrvFs cannot switch elevation context after VM creation");
    }

    if (elevated)
    {
        if (!m_adminDrvfsToken)
        {
            AddDrvFsShare(true, UserToken);
            THROW_IF_WIN32_BOOL_FALSE(
                ::DuplicateTokenEx(UserToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenImpersonation, &m_adminDrvfsToken));
        }
    }
    else
    {
        if (!m_drvfsToken)
        {
            AddDrvFsShare(false, UserToken);
            THROW_IF_WIN32_BOOL_FALSE(
                ::DuplicateTokenEx(UserToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenImpersonation, &m_drvfsToken));
        }
    }

    return elevated;
}

bool WslCoreVm::IsDnsTunnelingSupported() const
{
    WI_ASSERT(
        m_vmConfig.NetworkingMode == NetworkingMode::Nat || m_vmConfig.NetworkingMode == NetworkingMode::Mirrored ||
        m_vmConfig.NetworkingMode == NetworkingMode::Consomme);

    return SUCCEEDED_LOG(wsl::core::networking::DnsResolver::LoadDnsResolverMethods());
}

bool WslCoreVm::IsVhdAttached(_In_ PCWSTR VhdPath)
{
    auto lock = m_lock.lock_exclusive();
    const auto attachedDisks = m_backend->GetAttachedDisks();
    return std::any_of(attachedDisks.begin(), attachedDisks.end(), [VhdPath](const VmDiskAttachment& disk) {
        return !disk.PassThrough && wsl::windows::common::string::IsPathComponentEqual(disk.Path.c_str(), VhdPath);
    });
}

WslCoreVm::DiskMountResult WslCoreVm::MountDisk(
    _In_ PCWSTR Disk, _In_ DiskType MountDiskType, _In_ ULONG PartitionIndex, _In_opt_ PCWSTR Name, _In_opt_ PCWSTR Type, _In_opt_ PCWSTR Options)
{
    auto lock = m_lock.lock_exclusive();
    return MountDiskLockHeld(Disk, MountDiskType, PartitionIndex, Name, Type, Options);
}

WslCoreVm::DiskMountResult WslCoreVm::MountDiskLockHeld(
    _In_ PCWSTR Disk, _In_ DiskType MountDiskType, _In_ ULONG PartitionIndex, _In_opt_ PCWSTR Name, _In_opt_ PCWSTR Type, _In_opt_ PCWSTR Options)
{
    const auto disks = m_backend->GetAttachedDisks();
    const auto disk = std::find_if(disks.begin(), disks.end(), [&](const VmDiskAttachment& entry) {
        return entry.PassThrough == (MountDiskType == DiskType::PassThrough) &&
               wsl::windows::common::string::IsPathComponentEqual(entry.Path, Disk);
    });
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), disk == disks.end());

    auto& mountState = m_diskMounts[disk->Id.Value];
    THROW_HR_IF(WSL_E_DISK_ALREADY_MOUNTED, mountState.Mounts.contains(PartitionIndex));

    // Get the name for the mountpoint
    auto targetName = s_GetMountTargetName(Disk, Name, PartitionIndex);
    auto targetNameWide = wsl::shared::string::MultiByteToWide(targetName);
    // For each attachedDisk pair
    const auto nameCollision = std::any_of(m_diskMounts.begin(), m_diskMounts.end(), [&](const auto& diskEntry) {
        // Check if the targetName matches the name of any Mount already present
        return (std::any_of(diskEntry.second.Mounts.begin(), diskEntry.second.Mounts.end(), [&](const auto& mountEntry) {
            return wsl::shared::string::IsEqual(mountEntry.second.Name, targetNameWide, false);
        }));
    });

    // Throw error if the specified name was already used
    THROW_HR_IF(WSL_E_VM_MODE_MOUNT_NAME_ALREADY_EXISTS, nameCollision);

    wsl::shared::MessageWriter<LX_MINI_INIT_MOUNT_MESSAGE> message(LxMiniInitMessageMount);
    message->PartitionIndex = PartitionIndex;
    message->DeviceType = ToMiniInitDeviceType(disk->Transport);
    message->DeviceId = disk->GuestAddress.Lun;
    message.WriteString(message->TypeOffset, Type);
    message.WriteString(message->TargetNameOffset, targetName);
    message.WriteString(message->OptionsOffset, Options);

    // Send the message.
    auto transaction = m_miniInitChannel.StartTransaction();
    transaction.Send<LX_MINI_INIT_MOUNT_MESSAGE>(message.Span());

    // Accept a connection from mini_init
    wsl::shared::SocketChannel channel{AcceptConnection(m_vmConfig.KernelBootTimeout), "MountResult", {m_terminatingEvent.get()}};

    // Get the mount result from mini_init
    auto [mountResult, step] = GetMountResult(channel);
    if (mountResult == 0)
    {
        Mount mount;

        // Always set the Name attribute; use generated one as default
        mount.Name = std::move(targetNameWide);

        if (Type != nullptr)
        {
            mount.Type = Type;
        }

        if (Options != nullptr)
        {
            mount.Options = Options;
        }

        mountState.Mounts.emplace(PartitionIndex, std::move(mount));
    }

    return {std::move(targetName), mountResult, step};
}

wil::unique_socket WslCoreVm::CreateRootNamespaceProcess(_In_ LPCSTR Path, _In_ LPCSTR* Arguments)
{
    auto lock = m_lock.lock_exclusive();

    return LxssCreateProcess::CreateLinuxProcess(
        Path,
        Arguments,
        [this](ULONG Port, HANDLE ExitHandle) { return m_backend->ConnectGuest(GuestServicePort{Port}, ExitHandle); },
        m_miniInitChannel,
        m_terminatingEvent.get(),
        m_vmConfig.DistributionStartTimeout);
}

void WslCoreVm::MountRootNamespaceFolder(_In_ LPCWSTR HostPath, _In_ LPCWSTR GuestPath, _In_ bool ReadOnly, _In_ LPCWSTR Name)
{
    auto lock = m_lock.lock_exclusive();
    const bool useVirtioFs = m_backend->GetDescription().Backend == BackendKind::OpenVmm;
    std::wstring mountSource = Name;

    wsl::windows::common::security::EnableTokenPrivilege(m_userToken.get(), SE_CREATE_SYMBOLIC_LINK_NAME);
    if (useVirtioFs)
    {
        auto guestDeviceLock = m_guestDeviceLock.lock_exclusive();
        auto options = std::wstring{TEXT(LX_INIT_DEFAULT_PLAN9_MOUNT_OPTIONS)};
        if (ReadOnly)
        {
            options += L";ro";
        }

        std::wstring childName;
        std::tie(mountSource, childName, std::ignore) = AddVirtioFsShare(
            wsl::windows::common::security::IsTokenElevated(m_userToken.get()), HostPath, options.c_str(), m_userToken.get());
        THROW_HR_IF_MSG(E_UNEXPECTED, !childName.empty(), "OpenVMM plugin shares must use a single-share virtio-fs device");
    }
    else
    {
        const auto flags = (ReadOnly ? hcs::Plan9ShareFlags::ReadOnly : hcs::Plan9ShareFlags::None) | hcs::Plan9ShareFlags::AllowOptions;
        auto runAsUser = wil::impersonate_token(m_userToken.get());
        if (!m_pluginPlan9Server || m_pluginPlan9Server->IsRunning() != S_OK)
        {
            // Tear down the previous server so it releases the port before the replacement binds it.
            if (m_pluginPlan9Server)
            {
                LOG_IF_FAILED(m_pluginPlan9Server->Teardown());
                m_pluginPlan9Server.reset();
            }

            auto server =
                wsl::windows::common::wslutil::CreateComServerAsUser<p9fs::Plan9FileSystem, IPlan9FileSystem>(m_userToken.get());
            THROW_IF_FAILED(server->Init(&m_runtimeId, LX_INIT_UTILITY_VM_PLAN9_PLUGIN_PORT));
            THROW_IF_FAILED(server->Resume());
            m_pluginPlan9Server = std::move(server);
        }

        THROW_IF_FAILED(m_pluginPlan9Server->AddSharePath(Name, HostPath, static_cast<UINT32>(flags)));
    }

    wsl::shared::MessageWriter<LX_MINI_INIT_MOUNT_FOLDER_MESSAGE> message(LxMiniInitMountFolder);
    message.WriteString(message->PathIndex, GuestPath);
    message.WriteString(message->NameIndex, mountSource);
    message->ReadOnly = ReadOnly;
    message->VirtioFs = useVirtioFs;

    const auto& ResultMessage = m_miniInitChannel.Transaction<LX_MINI_INIT_MOUNT_FOLDER_MESSAGE>(message.Span());

    THROW_HR_IF_MSG(
        E_FAIL,
        ResultMessage.Result != 0,
        "Failed to mount folder. HostPath=%ls, GuestPath=%ls, Name=%ls, ReadOnly=%d, Result=%d",
        HostPath,
        GuestPath,
        Name,
        ReadOnly,
        ResultMessage.Result);
}

ULONG
WslCoreVm::MountFileAsPersistentMemory(_In_ PCWSTR FilePath, _In_ bool ReadOnly)
{
    VmPersistentMemoryRequest request{};
    request.Path = FilePath;
    request.ReadOnly = ReadOnly;
    if (m_backend->GetDescription().Backend == BackendKind::Hcs)
    {
        request.UserToken = m_userToken;
    }
    request.WaitForGuestDevice = [this](std::uint32_t index) { WaitForPmemDeviceInVm(index); };
    return m_backend->AddPersistentMemory(request).Index;
}

void WslCoreVm::WaitForPmemDeviceInVm(_In_ ULONG PmemId)
{
    // Construct the mini_init message.
    LX_MINI_INIT_WAIT_FOR_PMEM_DEVICE_MESSAGE message{};
    message.Header.MessageType = LxMiniInitMessageWaitForPmemDevice;
    message.Header.MessageSize = sizeof(message);
    message.PmemId = PmemId;

    // Send the message to mini_init.
    wsl::shared::SocketChannel channel;
    {
        auto lock = m_lock.lock_exclusive();

        auto transaction = m_miniInitChannel.StartTransaction();
        transaction.Send(message);
        channel = {
            AcceptConnection(m_vmConfig.KernelBootTimeout),
            "WaitForPmem",
            {m_terminatingEvent.get()},
        };
    }

    // Wait for mini_init to respond.

    const auto& resultMessage = channel.ReceiveMessage<LX_MINI_INIT_WAIT_FOR_PMEM_DEVICE_MESSAGE::TResponse>();

    // Check if the device was found in the VM.
    if (resultMessage.Result != 0)
    {
        THROW_WIN32_MSG(ERROR_NOT_FOUND, "Failed to find /dev/pmem%u with result %d", PmemId, resultMessage.Result);
    }
}

_Requires_lock_held_(m_guestDeviceLock)
std::tuple<std::wstring, std::wstring, std::wstring> WslCoreVm::AddVirtioFsShare(_In_ bool Admin, _In_ PCWSTR Path, _In_ PCWSTR Options, _In_opt_ HANDLE UserToken)
{
    WI_ASSERT(m_vmConfig.EnableVirtioFs);

    if (!ARGUMENT_PRESENT(UserToken))
    {
        UserToken = Admin ? m_adminDrvfsToken.get() : m_drvfsToken.get();
        THROW_HR_IF_MSG(E_UNEXPECTED, !UserToken, "UserToken not set for supplied context (Admin = %d)", Admin);
    }

    WI_ASSERT(Admin == wsl::windows::common::security::IsTokenElevated(UserToken));

    // Ensure that the path has a trailing path separator.
    std::wstring sharePath = NormalizeSharePath(Path);
    const auto mountOptions = ParseVirtioFsMountOptions(Options);
    const bool openVmm = m_backend->GetDescription().Backend == BackendKind::OpenVmm;
    const bool readOnly = openVmm && mountOptions.contains(L"ro");

    bool created = false;
    auto share = m_backend->GetFileSystemShare([&](const VmFileSystemShare& candidate) {
        return std::holds_alternative<VmVirtioFsShareAddress>(candidate.GuestAddress) && candidate.Elevated == Admin &&
               candidate.ReadOnly == readOnly && candidate.EffectiveHostPath.native() == sharePath && candidate.MountOptions == mountOptions;
    });
    if (!share)
    {
        // Generate a new unique tag for the share.
        //
        // N.B. The tag can be maximum 36 characters long so a GUID without braces fits perfectly.
        GUID tagGuid{};
        THROW_IF_FAILED(CoCreateGuid(&tagGuid));

        const auto shareName = wsl::shared::string::GuidToString<wchar_t>(tagGuid, wsl::shared::string::None);
        WI_ASSERT(!FindVirtioFsShare(shareName.c_str(), Admin));

        VmFileSystemDevice device;
        if (m_vmConfig.EnableVirtioFsAggregateShares && !openVmm)
        {
            const PCWSTR deviceTag = Admin ? TEXT(LX_INIT_DRVFS_ADMIN_VIRTIO_TAG) : TEXT(LX_INIT_DRVFS_VIRTIO_TAG);
            const auto existingDevice = m_backend->GetFileSystemDevice([&](const VmFileSystemDevice& candidate) {
                const auto* virtio = std::get_if<VmVirtioFsDevice>(&candidate.Transport);
                return virtio && virtio->Layout == VmVirtioFsLayout::Aggregate && virtio->Tag == deviceTag && candidate.Elevated == Admin;
            });
            if (existingDevice)
            {
                device = existingDevice.value();
            }
            else
            {
                VmFileSystemDeviceRequest request{};
                request.Transport = VmVirtioFsDevice{deviceTag, VmVirtioFsLayout::Aggregate};
                request.UserToken = wil::shared_handle{wsl::windows::common::wslutil::DuplicateHandle(UserToken)};
                device = m_backend->CreateFileSystemDevice(request);
            }

            VmFileSystemShareRequest request{};
            request.HostPath = sharePath;
            request.Name = shareName;
            request.ReadOnly = false;
            request.Options = VmVirtioFsShareOptions{mountOptions};
            share = m_backend->AddFileSystemShare(device.Id, request);
        }
        else
        {
            VmFileSystemDeviceRequest deviceRequest{};
            deviceRequest.Transport = VmVirtioFsDevice{shareName, VmVirtioFsLayout::SingleShare};
            if (!openVmm)
            {
                deviceRequest.UserToken = wil::shared_handle{wsl::windows::common::wslutil::DuplicateHandle(UserToken)};
            }
            device = m_backend->CreateFileSystemDevice(deviceRequest);

            VmFileSystemShareRequest request{};
            request.HostPath = sharePath;
            request.ReadOnly = readOnly;
            request.Options = VmVirtioFsShareOptions{mountOptions};
            share = m_backend->AddFileSystemShare(device.Id, request);
        }

        created = true;
    }

    const auto& address = std::get<VmVirtioFsShareAddress>(share->GuestAddress);
    const auto childName = address.ChildName.value_or(L"");

    WSL_LOG(
        "WslCoreVmAddVirtioFsShare",
        TraceLoggingValue(Admin, "admin"),
        TraceLoggingValue(sharePath.c_str(), "path"),
        TraceLoggingValue(Options, "options"),
        TraceLoggingValue(address.Tag.c_str(), "tag"),
        TraceLoggingValue(childName.c_str(), "childName"),
        TraceLoggingValue(m_vmConfig.EnableVirtioFsAggregateShares && !openVmm, "aggregate"),
        TraceLoggingValue(created, "created"));

    return {address.Tag, childName, share->EffectiveHostPath.native()};
}

void WslCoreVm::OnExit(const VmTerminationInformation& Termination)
{
    // Indicate that the VM has exited and wake any waiting threads. The backend owns and drains the
    // underlying platform callbacks before it is destroyed.
    std::function<void(GUID)> terminationCallback{};
    {
        auto exitLock = m_exitCallbackLock.lock_exclusive();
        m_terminationInformation = Termination;
        m_vmExitEvent.SetEvent();
        if (Termination.Reason == VmTerminationReason::Crashed)
        {
            m_vmCrashEvent.SetEvent();
        }

        // If we reach this block and 'm_terminatingEvent' is not signaled, then this is abnormal shutdown.
        // If that happens, set m_terminatingEvent so all pending socket operations can be properly cancelled.
        if (!m_terminatingEvent.is_signaled())
        {
            WSL_LOG("AbnormalVmExit", TraceLoggingValue(Termination.Details.c_str(), "Details"));
            m_terminatingEvent.SetEvent();
        }

        terminationCallback = std::move(m_onExit);
    }

    if (terminationCallback)
    {
        terminationCallback(m_runtimeId);
    }
}

void WslCoreVm::ReadGuestCapabilities()
{
    gsl::span<gsl::byte> span;
    const auto& info = m_miniInitChannel.ReceiveMessage<LX_INIT_GUEST_CAPABILITIES>(&span);
    const std::string input{wsl::shared::string::FromMessageBuffer<LX_INIT_GUEST_CAPABILITIES>(span)};

    m_kernelVersionString = wsl::shared::string::MultiByteToWide(input);

    // Parse the version string.
    const std::regex pattern("(\\d+)\\.(\\d+)\\.(\\d+).*");
    std::smatch match;
    if (!std::regex_match(input, match, pattern) || match.size() != 4)
    {
        THROW_HR_MSG(E_UNEXPECTED, "Failed to parse kernel version: '%hs'", input.c_str());
    }

    auto get = [&](int position) { return std::stoul(match.str(position)); };

    try
    {
        m_kernelVersion = std::make_tuple(get(1), get(2), get(3));
    }
    catch (const std::exception& e)
    {
        THROW_HR_MSG(E_UNEXPECTED, "Failed to parse kernel version: '%hs', %hs", input.c_str(), e.what());
    }

    m_seccompAvailable = info.SeccompAvailable;
    m_hvPciSwiotlbBase = info.HvPciSwiotlbBase;
    m_hvPciSwiotlbSize = info.HvPciSwiotlbSize;
    WSL_LOG(
        "GuestKernelInfo",
        TraceLoggingValue(m_seccompAvailable, "SeccompAvailable"),
        TraceLoggingValue(m_hvPciSwiotlbBase, "HvPciSwiotlbBase"),
        TraceLoggingValue(m_hvPciSwiotlbSize, "HvPciSwiotlbSize"),
        TraceLoggingValue(std::get<0>(m_kernelVersion), "Version"),
        TraceLoggingValue(std::get<1>(m_kernelVersion), "Revision"),
        TraceLoggingValue(std::get<2>(m_kernelVersion), "Minor"));
}

void WslCoreVm::RegisterCallbacks(_In_ const std::function<void(ULONG)>& DistroExitCallback, _In_ const std::function<void(GUID)>& TerminationCallback)
{
    WSL_LOG(
        "WslCoreVm::RegisterCallbacks",
        TraceLoggingValue(static_cast<bool>(DistroExitCallback), "DistroExitCallback"),
        TraceLoggingValue(static_cast<bool>(TerminationCallback), "TerminationCallback"));

    if (DistroExitCallback)
    {
        auto lock = m_lock.lock_exclusive();
        THROW_HR_IF(E_INVALIDARG, !m_notifyChannel);
        m_distroExitThread = std::thread([exitCallback = std::move(DistroExitCallback),
                                          notifyChannel = std::move(m_notifyChannel),
                                          terminationEvent = m_terminatingEvent.get()]() {
            try
            {
                wsl::windows::common::wslutil::SetThreadDescription(L"DistroExitCallback");

                std::vector<gsl::byte> buffer;
                for (;;)
                {
                    // Read the message.
                    auto message = wsl::shared::socket::RecvMessage(notifyChannel.get(), buffer, terminationEvent);
                    if (message.empty())
                    {
                        break;
                    }

                    const auto* header = gslhelpers::get_struct<MESSAGE_HEADER>(message);
                    if (header->MessageType == LxMiniInitMessageChildExit)
                    {
                        const auto* exitMessage = gslhelpers::try_get_struct<LX_MINI_INIT_CHILD_EXIT_MESSAGE>(message);
                        if (exitMessage)
                        {
                            WSL_LOG("ProcessExited", TraceLoggingValue(exitMessage->ChildPid, "pid"));
                            exitCallback(exitMessage->ChildPid);
                        }
                    }
                    else
                    {
                        LOG_HR_MSG(E_UNEXPECTED, "Unexpected MessageType %d", header->MessageType);
                    }
                }
            }
            CATCH_LOG()
        });
    }

    if (TerminationCallback)
    {
        // Register the callback if the VM has not been terminated.
        auto exitLock = m_exitCallbackLock.lock_exclusive();
        THROW_HR_IF(E_INVALIDARG, m_onExit);
        if (!m_terminatingEvent.is_signaled())
        {
            m_onExit = std::move(TerminationCallback);
        }
        else
        {
            // The VM has already been terminated, invoke the callback on a separate thread.
            std::thread([terminationCallback = std::move(TerminationCallback), runtimeId = m_runtimeId]() {
                wsl::windows::common::wslutil::SetThreadDescription(L"TerminationCallback");
                terminationCallback(runtimeId);
            }).detach();
        }
    }

    if (m_vmConfig.EnableHostFileSystemAccess && m_vmConfig.EnableVirtioFs)
    {
        // Create a thread listening for handling virtiofs requests.
        m_virtioFsListener = m_backend->CreateGuestListener(GuestServicePort{LX_INIT_UTILITY_VM_VIRTIOFS_PORT});
        m_virtioFsThread = std::thread(&WslCoreVm::VirtioFsWorker, this, m_virtioFsListener.value());
    }
}

void WslCoreVm::ResizeDistribution(_In_ ULONG Lun, _In_ HANDLE OutputHandle, _In_ ULONG64 NewSize)
{
    auto lock = m_lock.lock_exclusive();
    const auto disk = FindAttachedDiskByLun(*m_backend, Lun);

    LX_MINI_INIT_RESIZE_DISTRIBUTION_MESSAGE message{};
    message.Header.MessageSize = sizeof(message);
    message.Header.MessageType = LxMiniInitMessageResizeDistribution;
    message.DeviceType = ToMiniInitDeviceType(disk.Transport);
    message.DeviceId = disk.GuestAddress.Lun;
    message.NewSize = NewSize;

    auto transaction = m_miniInitChannel.StartTransaction();
    transaction.Send(message);

    wsl::shared::SocketChannel channel{AcceptConnection(m_vmConfig.KernelBootTimeout), "ResizeDistribution", {m_terminatingEvent.get()}};
    auto outputChannel = AcceptConnection(m_vmConfig.KernelBootTimeout);

    wsl::windows::common::relay::ScopedRelay outputRelay(std::move(outputChannel), OutputHandle);

    const auto& resultMessage = channel.ReceiveMessage<LX_MINI_INIT_RESIZE_DISTRIBUTION_RESPONSE>();
    if (resultMessage.ResponseCode != 0)
    {
        THROW_HR_WITH_USER_ERROR(E_FAIL, wsl::shared::Localization::MessageFailedToResizeDisk());
    }
}

void WslCoreVm::TrimDistribution(_In_ ULONG Lun)
{
    auto lock = m_lock.lock_exclusive();
    const auto disk = FindAttachedDiskByLun(*m_backend, Lun);

    LX_MINI_INIT_TRIM_DISTRIBUTION_MESSAGE message{};
    message.Header.MessageSize = sizeof(message);
    message.Header.MessageType = LxMiniInitMessageTrimDistribution;
    message.DeviceType = ToMiniInitDeviceType(disk.Transport);
    message.DeviceId = disk.GuestAddress.Lun;

    auto transaction = m_miniInitChannel.StartTransaction();
    transaction.Send(message);

    wsl::shared::SocketChannel channel{AcceptConnection(m_vmConfig.KernelBootTimeout), "TrimDistribution", {m_terminatingEvent.get()}};

    const auto& resultMessage = channel.ReceiveMessage<LX_MINI_INIT_TRIM_DISTRIBUTION_RESPONSE>();
    THROW_HR_IF(E_FAIL, resultMessage.ResponseCode != 0);
}

void WslCoreVm::SaveAttachedDisksState()
try
{
    auto lock = m_lock.lock_exclusive();
    const auto key = wsl::windows::common::registry::OpenOrCreateLxssDiskMountsKey(&m_userSid.Sid);
    for (const auto& disk : m_backend->GetAttachedDisks())
    {
        if (!disk.UserDisk)
        {
            continue;
        }

        const auto mountState = m_diskMounts.find(disk.Id.Value);
        SaveDiskState(key.get(), disk, mountState != m_diskMounts.end() ? mountState->second : DiskMountState{});
    }

    return;
}
CATCH_LOG()

void WslCoreVm::SaveDiskState(_In_ HKEY Key, _In_ const VmDiskAttachment& Disk, _In_ const DiskMountState& State)
{
    const auto keyPath = std::to_wstring(Disk.GuestAddress.Lun);
    const auto diskKey = wsl::windows::common::registry::CreateKey(Key, keyPath.c_str(), KEY_ALL_ACCESS, nullptr, REG_OPTION_VOLATILE);

    wsl::windows::common::registry::WriteString(diskKey.get(), nullptr, c_diskValueName, Disk.Path.c_str());

    const auto diskType = Disk.PassThrough ? DiskType::PassThrough : DiskType::VHD;
    wsl::windows::common::registry::WriteDword(diskKey.get(), nullptr, c_disktypeValueName, static_cast<DWORD>(diskType));

    for (const auto& e : State.Mounts)
    {
        auto partition = std::to_wstring(e.first);
        auto mountKey = wsl::windows::common::registry::CreateKey(diskKey.get(), partition.c_str(), KEY_ALL_ACCESS, nullptr, REG_OPTION_VOLATILE);

        wsl::windows::common::registry::WriteString(mountKey.get(), nullptr, c_mountNameValueName, e.second.Name.c_str());

        if (e.second.Options.has_value())
        {
            wsl::windows::common::registry::WriteString(mountKey.get(), nullptr, c_optionsValueName, e.second.Options.value().c_str());
        }

        if (e.second.Type.has_value())
        {
            wsl::windows::common::registry::WriteString(mountKey.get(), nullptr, c_typeValueName, e.second.Type.value().c_str());
        }
    }
}

std::pair<int, LX_MINI_MOUNT_STEP> WslCoreVm::UnmountDisk(_In_ const VmDiskAttachment& Disk, _Inout_ DiskMountState& State)
{
    // Iterate through the mountpoints to unmount and delete them
    for (auto it = State.Mounts.begin(); it != State.Mounts.end(); it = State.Mounts.erase(it))
    {
        const auto result = UnmountVolume(it->second.Name.c_str());
        if (result.first != 0)
        {
            return result;
        }
    }

    // Tell the guest to flush its IO caches and stop using the disk.
    LX_MINI_INIT_DETACH_MESSAGE message{};
    message.Header.MessageType = LxMiniInitMessageDetach;
    message.Header.MessageSize = sizeof(message);
    message.DeviceType = ToMiniInitDeviceType(Disk.Transport);
    message.DeviceId = Disk.GuestAddress.Lun;

    auto transaction = m_miniInitChannel.StartTransaction();
    transaction.Send(message);

    // Accept a connection from mini_init.
    wsl::shared::SocketChannel channel{AcceptConnection(m_vmConfig.KernelBootTimeout), "MountResult", {m_terminatingEvent.get()}};

    // Get the unmount result from mini_init
    return GetMountResult(channel);
}

std::pair<int, LX_MINI_MOUNT_STEP> WslCoreVm::UnmountVolume(_In_ PCWSTR Name)
{
    wsl::shared::MessageWriter<LX_MINI_INIT_UNMOUNT_MESSAGE> message(LxMiniInitMessageUnmount);
    message.WriteString(Name);

    // Send the message.
    auto transaction = m_miniInitChannel.StartTransaction();
    transaction.Send<LX_MINI_INIT_UNMOUNT_MESSAGE>(message.Span());

    // Accept a connection from mini_init.
    wsl::shared::SocketChannel channel{AcceptConnection(m_vmConfig.KernelBootTimeout), "MountResult", {m_terminatingEvent.get()}};

    // Get the unmount result from mini_init.
    return GetMountResult(channel);
}

_Requires_lock_held_(m_guestDeviceLock)
void WslCoreVm::VerifyPlan9Servers()
{
    for (const auto port : {LX_INIT_UTILITY_VM_PLAN9_DRVFS_ADMIN_PORT, LX_INIT_UTILITY_VM_PLAN9_DRVFS_PORT})
    {
        const auto device = m_backend->GetFileSystemDevice([&](const VmFileSystemDevice& candidate) {
            const auto* socket = std::get_if<VmPlan9SocketDevice>(&candidate.Transport);
            return socket && socket->Port.Value == port;
        });

        if (!device)
        {
            continue;
        }

        const auto status = m_backend->GetFileSystemDeviceStatus(device->Id);
        if (status.State != VmFileSystemDeviceState::Serving)
        {
            m_backend->RemoveDevice(device->Id);
            if (port == LX_INIT_UTILITY_VM_PLAN9_DRVFS_ADMIN_PORT)
            {
                m_adminDrvfsToken.reset();
            }
            else
            {
                m_drvfsToken.reset();
            }
        }
    }
}

void WslCoreVm::VirtioFsWorker(VmGuestListener Listener)
try
{
    wsl::windows::common::wslutil::SetThreadDescription(L"VirtioFs - Worker");

    io::MultiHandleWait io;

    io.AddHandle(std::make_unique<io::AcceptHandle>(Listener.Socket(), false, [this, &io](wil::unique_socket&& socket) {
        auto channel = std::make_shared<wsl::shared::SocketChannel>(std::move(socket), "VirtioFs");
        auto buffer = std::make_shared<std::vector<gsl::byte>>();
        auto pendingBytes = std::make_shared<std::vector<gsl::byte>>();

        io.AddHandle(
            std::make_unique<io::ReadSocketMessageHandle>(
                io::HandleWrapper(channel->Socket()),
                *buffer,
                *pendingBytes,
                [this, &io, channel, buffer, pendingBytes](const gsl::span<gsl::byte>& message) {
                    if (message.empty())
                    {
                        return; // Channel closed, exit.
                    }

                    THROW_HR_IF_MSG(
                        E_UNEXPECTED, !pendingBytes->empty(), "Received message with additional bytes: %zu", pendingBytes->size());

                    try
                    {
                        auto response = ProcessVirtioFsRequest(message);

                        // Move the socket out of the channel into the WriteHandle so it is closed once the reply is sent.
                        io.AddHandle(std::make_unique<io::WriteHandle>(channel->Release(), response), io::MultiHandleWait::IgnoreErrors);
                    }
                    CATCH_LOG();
                }),
            io::MultiHandleWait::IgnoreErrors);
    }));

    io.AddHandle(std::make_unique<io::EventHandle>(m_terminatingEvent.get()), io::MultiHandleWait::CancelOnCompleted);

    io.Run({});
}
CATCH_LOG()

std::vector<char> WslCoreVm::ProcessVirtioFsRequest(_In_ gsl::span<gsl::byte> Request)
{
    const auto* header = gslhelpers::try_get_struct<MESSAGE_HEADER>(Request);
    THROW_HR_IF(E_UNEXPECTED, !header);

    WSL_LOG("VirtiofsMessageRequest", TraceLoggingValue(header->PrettyPrint().c_str(), "Content"));

    auto buildResponse = [header](const std::wstring& tag, const std::wstring& childName, const std::wstring& source, HRESULT result) {
        // Respond to the guest with the tag that should be used to mount the device.
        wsl::shared::MessageWriter<LX_INIT_ADD_VIRTIOFS_SHARE_RESPONSE_MESSAGE> response(LxInitMessageAddVirtioFsDeviceResponse);
        response->Result = SUCCEEDED(result) ? 0 : EINVAL; // TODO: Improved HRESULT -> errno mapping.
        response.WriteString(response->TagOffset, tag);
        response.WriteString(response->ChildNameOffset, childName);
        response.WriteString(response->SourceOffset, source);

        // Echo the request's transaction id and mark the message as the first (and only) reply.
        response->Header.TransactionId = header->TransactionId;
        response->Header.TransactionStep = static_cast<unsigned int>(TRANSACTION_STEP::FIRST_REPLY);

        WSL_LOG("VirtiofsMessageResponse", TraceLoggingValue(response->PrettyPrint().c_str(), "Content"));

        const auto span = response.Span();
        return std::vector<char>(reinterpret_cast<const char*>(span.data()), reinterpret_cast<const char*>(span.data()) + span.size());
    };

    if (header->MessageType == LxInitMessageAddVirtioFsDevice)
    {
        std::wstring tag;
        std::wstring childName;
        std::wstring source;
        const auto result = wil::ResultFromException([&]() {
            const auto* addShare = gslhelpers::try_get_struct<LX_INIT_ADD_VIRTIOFS_SHARE_MESSAGE>(Request);
            THROW_HR_IF(E_UNEXPECTED, !addShare);

            const auto path = wsl::shared::string::FromSpan(Request, addShare->PathOffset);
            const auto pathWide = wsl::shared::string::MultiByteToWide(path);
            const auto options = wsl::shared::string::FromSpan(Request, addShare->OptionsOffset);
            const auto optionsWide = wsl::shared::string::MultiByteToWide(options);

            // Acquire the lock and attempt to add the device.
            auto guestDeviceLock = m_guestDeviceLock.lock_exclusive();
            std::tie(tag, childName, source) = AddVirtioFsShare(addShare->Admin, pathWide.c_str(), optionsWide.c_str());
        });

        return buildResponse(tag, childName, source, result);
    }
    else if (header->MessageType == LxInitMessageRemountVirtioFsDevice)
    {
        std::wstring newTag;
        std::wstring childName;
        std::wstring source;
        const auto result = wil::ResultFromException([&]() {
            const auto* remountShare = gslhelpers::try_get_struct<LX_INIT_REMOUNT_VIRTIOFS_SHARE_MESSAGE>(Request);
            THROW_HR_IF(E_UNEXPECTED, !remountShare);

            const std::string tag = wsl::shared::string::FromSpan(Request, remountShare->TagOffset);
            const auto tagWide = wsl::shared::string::MultiByteToWide(tag);
            auto guestDeviceLock = m_guestDeviceLock.lock_exclusive();
            const auto foundShare = FindVirtioFsShare(tagWide.c_str(), !remountShare->Admin);
            THROW_HR_IF_MSG(E_UNEXPECTED, !foundShare.has_value(), "Unknown tag %ls", tagWide.c_str());

            const auto options = FormatVirtioFsMountOptions(foundShare->MountOptions);
            std::tie(newTag, childName, source) =
                AddVirtioFsShare(remountShare->Admin, foundShare->EffectiveHostPath.c_str(), options.c_str());

            WI_ASSERT(source == foundShare->EffectiveHostPath);
        });

        return buildResponse(newTag, childName, source, result);
    }
    else
    {
        THROW_HR_MSG(E_UNEXPECTED, "Unexpected MessageType %d", header->MessageType);
    }
}

std::string WslCoreVm::s_GetMountTargetName(_In_ PCWSTR Disk, _In_opt_ PCWSTR Name, _In_ int PartitionIndex)
{
    // Derive the mount target from the disk and partition names.
    // The format is <Disk>p[partition]
    // For Example: PhysicalDisk1p2
    // If user has specified the name, ensure proper formatting and use it instead
    if (ARGUMENT_PRESENT(Name))
    {
        auto mountName = wsl::shared::string::WideToMultiByte(Name);
        THROW_HR_IF(
            WSL_E_VM_MODE_INVALID_MOUNT_NAME,
            mountName.empty() || mountName == "." || mountName == ".." || mountName.find('/') != std::string::npos);
        return mountName;
    }

    std::string target{};
    auto mountName = wsl::shared::string::WideToMultiByte(Disk);
    std::copy_if(mountName.begin(), mountName.end(), std::back_inserter(target), &isalnum);
    if (PartitionIndex != 0)
    {
        target += std::format("p{}", PartitionIndex);
    }

    return target;
}

void WslCoreVm::TraceLoggingRundown() const noexcept
try
{
    WSL_LOG(
        "WslCoreVm::Rundown",
        TraceLoggingValue("Machine Config"),
        TraceLoggingValue(m_machineId.c_str(), "machineId"),
        TraceLoggingValue(ToString(m_vmConfig.NetworkingMode), "networkingMode"));

    if (m_networkingEngine)
    {
        m_networkingEngine->TraceLoggingRundown();
    }
}
CATCH_LOG()

void WslCoreVm::ValidateNetworkingMode()
{
    using namespace wsl::core;
    using namespace wsl::windows::common;

    ExecutionContext context(Context::ConfigureNetworking);

    // Cache requested networking features to be logged via telemetry.
    const auto networkingModeRequested = m_vmConfig.NetworkingMode;
    auto firewallRequested = m_vmConfig.FirewallConfig.Enabled();
    auto dnsTunnelingRequested = m_vmConfig.EnableDnsTunneling;

    // If Hyper-V firewall was requested, ensure it is supported by the OS.
    if (m_vmConfig.FirewallConfig.Enabled())
    {
        if (m_vmConfig.NetworkingMode == NetworkingMode::Mirrored || m_vmConfig.NetworkingMode == NetworkingMode::Nat)
        {
            if (!wsl::core::MirroredNetworking::IsHyperVFirewallSupported(m_vmConfig))
            {
                // Since hyper-V firewall is enabled by default, only show the warning if the user explicitly asked for it.
                if (m_vmConfig.FirewallConfigPresence == ConfigKeyPresence::Present)
                {
                    EMIT_USER_WARNING(Localization::MessageHyperVFirewallNotSupported());
                }

                m_vmConfig.FirewallConfig.reset();
            }
        }
    }

    // If mirrored networking was requested, ensure IPv6 is not disabled on the host using registry,
    // as this is not supported by mirrored networking.
    // Note: Disabling IPv6 using Set-NetAdapterBinding is supported.
    if (m_vmConfig.NetworkingMode == NetworkingMode::Mirrored)
    {
        constexpr DWORD c_ipv6Disabled = 0xFF;
        DWORD disabledComponents = 0;
        wil::reg::get_value_dword_nothrow(
            HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters", L"DisabledComponents", &disabledComponents);

        if (disabledComponents == c_ipv6Disabled)
        {
            m_vmConfig.NetworkingMode = NetworkingMode::Nat;
            EMIT_USER_WARNING(Localization::MessageMirroredNetworkingNotSupportedReason(
                Localization::MessageMirroredNetworkingNotSupportedIpv6Disabled()));
        }
    }

    // If mirrored networking was requested, ensure it is supported by the OS and guest kernel.
    if (m_vmConfig.NetworkingMode == NetworkingMode::Mirrored)
    {
        if ((m_kernelVersion < std::make_tuple(5u, 10u, 0u)) || !m_seccompAvailable)
        {
            m_vmConfig.NetworkingMode = NetworkingMode::Nat;
            EMIT_USER_WARNING(Localization::MessageMirroredNetworkingNotSupportedReason(
                Localization::MessageMirroredNetworkingNotSupportedKernel()));
        }
        else if (!wsl::core::networking::IsFlowSteeringSupportedByHns() || !m_vmConfig.FirewallConfig.Enabled())
        {
            m_vmConfig.NetworkingMode = NetworkingMode::Nat;
            EMIT_USER_WARNING(Localization::MessageMirroredNetworkingNotSupportedReason(Localization::MessageMirroredNetworkingNotSupportedWindowsVersion(
                m_windowsVersion.BuildNumber, m_windowsVersion.UpdateBuildRevision)));
        }
    }

    // Localhost relay is not supported in mirrored mode. Generate a warning if the user configures localhost relay
    // together with mirrored mode.
    // N.B. Mirrored mode already provides a way to communicate between Windows and Linux using localhost.
    if (m_vmConfig.NetworkingMode == NetworkingMode::Mirrored && m_vmConfig.LocalhostRelayConfigPresence == ConfigKeyPresence::Present)
    {
        EMIT_USER_WARNING(Localization::MessageLocalhostForwardingNotSupportedMirroredMode());
    }

    // The DnsResolver support check still applies to Consomme because the host Consomme NAT uses the same Windows DNS APIs.
    if (m_vmConfig.EnableDnsTunneling && !IsDnsTunnelingSupported())
    {
        // Since DNS tunneling is enabled by default, only show the warning if the user explicitly asked for it.
        if (m_vmConfig.DnsTunnelingConfigPresence == ConfigKeyPresence::Present)
        {
            EMIT_USER_WARNING(Localization::MessageDnsTunnelingNotSupported());
        }

        m_vmConfig.EnableDnsTunneling = false;
    }

    // Gives information about the requested networking settings and whether they were enabled or not
    WSL_LOG_TELEMETRY(
        "WslCoreVmValidateNetworkingMode",
        PDT_ProductAndServicePerformance,
        TraceLoggingValue(m_runtimeId, "vmId"),
        TraceLoggingValue(ToString(networkingModeRequested), "networkingModeRequested"),
        TraceLoggingValue(ToString(m_vmConfig.NetworkingMode), "networkingMode"),
        TraceLoggingValue(m_vmConfig.NetworkingModePresence == ConfigKeyPresence::Present, "networkingModePresent"),
        TraceLoggingValue(firewallRequested, "firewallRequested"),
        TraceLoggingValue(m_vmConfig.FirewallConfig.Enabled(), "firewall"),
        TraceLoggingValue(dnsTunnelingRequested, "dnsTunnelingRequested"),
        TraceLoggingValue(m_vmConfig.DnsTunnelingConfigPresence == ConfigKeyPresence::Present, "dnsTunnelingConfigPresent"),
        TraceLoggingValue(m_vmConfig.EnableDnsTunneling, "dnsTunneling"));
}
