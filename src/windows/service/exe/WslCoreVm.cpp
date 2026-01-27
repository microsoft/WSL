/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslCoreVm.cpp

Abstract:

    This file contains utility VM function definitions.

--*/

#include "precomp.h"
#include "WslCoreVm.h"
#include "WslCoreNetworkingSupport.h"
#include <lxfsshares.h>
#include "disk.hpp"
#include "WslCoreInstance.h"
#include "NatNetworking.h"
#include "BridgedNetworking.h"
#include "MirroredNetworking.h"
#include "WslCoreFirewallSupport.h"
#include "DnsResolver.h"
#include "VirtioNetworking.h"

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

// Version numbers for various functionality that was backported.
#define NICKEL_BUILD_FLOOR 22350
#define VIRTIO_SERIAL_CONSOLE_COBALT_RELEASE_UBR 40
#define VMEMM_SUFFIX_COBALT_REFRESH_BUILD_NUMBER 22138
#define VMMEM_SUFFIX_COBALT_RELEASE_UBR 71
#define VMMEM_SUFFIX_NICKEL_BUILD_NUMBER 22420

#define WSLG_SHARED_MEMORY_SIZE_MB 8192
#define PAGE_SIZE 0x1000

static constexpr size_t c_bootEntropy = 0x1000;
static constexpr auto c_localDevicesKey = L"SOFTWARE\\Microsoft\\Terminal Server Client\\LocalDevices";
static constexpr std::pair<uint32_t, uint32_t> c_schemaVersionNickel{2, 7};

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
} // namespace

WslCoreVm::WslCoreVm(_In_ wsl::core::Config&& VmConfig) :
    m_vmConfig(std::move(VmConfig)), m_traceClient(m_vmConfig.EnableTelemetry)
{
}

std::unique_ptr<WslCoreVm> WslCoreVm::Create(_In_ const wil::shared_handle& UserToken, _In_ wsl::core::Config&& VmConfig, _In_ const GUID& VmId)
{
    auto newInstance = std::unique_ptr<WslCoreVm>{new WslCoreVm{std::move(VmConfig)}};
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
                if (newInstance->m_vmCrashLogFile.has_value())
                {
                    THROW_HR_WITH_USER_ERROR(
                        WSL_E_VM_CRASHED,
                        wsl::shared::Localization::MessageWSL2Crashed() + L"\r\n" +
                            Localization::MessageWSL2CrashedStackTrace(newInstance->m_vmCrashLogFile.value()));
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

    // Initialize the path to the tools folder.
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
    if (m_vmConfig.KernelModulesPath.empty())
    {
        if (m_defaultKernel)
        {
#ifdef WSL_KERNEL_MODULES_PATH

            m_vmConfig.KernelModulesPath = std::wstring(TEXT(WSL_KERNEL_MODULES_PATH));

#else

            m_vmConfig.KernelModulesPath = m_rootFsPath / L"modules.vhd";

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
    m_vmConfig.EnableDebugShell &= IsVirtioSerialConsoleSupported();
    if (IsVirtioSerialConsoleSupported())
    {
        try
        {
            bool enableTelemetry = TraceLoggingProviderEnabled(g_hTraceLoggingProvider, WINEVENT_LEVEL_INFO, 0);
            m_dmesgCollector = DmesgCollector::Create(
                VmId, m_vmExitEvent, enableTelemetry, m_vmConfig.EnableDebugConsole, m_comPipe0, m_vmConfig.EnableEarlyBootLogging);

            WSL_LOG("DMESG collector created");

            if (m_vmConfig.EnableDebugShell)
            {
                m_debugShellPipe = wsl::windows::common::wslutil::GetDebugShellPipeName(&m_userSid.Sid);
            }

            // Initialize the guest telemetry logger.
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
                m_comPipe0.c_str(), !!m_dmesgCollector, m_restrictedToken.get(), logFile ? logFile.get() : nullptr, !m_vmConfig.EnableTelemetry);
        }
        CATCH_LOG()
    }

    // Create the utility VM and store the runtime ID.
    std::wstring json = GenerateConfigJson();
    m_system = wsl::windows::common::hcs::CreateComputeSystem(m_machineId.c_str(), json.c_str());
    m_runtimeId = wsl::windows::common::hcs::GetRuntimeId(m_system.get());
    WI_ASSERT(IsEqualGUID(VmId, m_runtimeId));

    // Initialize the guest device manager.
    m_guestDeviceManager = std::make_shared<GuestDeviceManager>(m_machineId, m_runtimeId);

    // Create a socket listening for connections from mini_init.
    m_listenSocket = wsl::windows::common::hvsocket::Listen(m_runtimeId, LX_INIT_UTILITY_VM_INIT_PORT);

    if (m_vmConfig.MaxCrashDumpCount >= 0)
    {
        auto crashDumpSocket = wsl::windows::common::hvsocket::Listen(m_runtimeId, LX_INIT_UTILITY_VM_CRASH_DUMP_PORT);
        THROW_LAST_ERROR_IF(!crashDumpSocket);
        m_crashDumpCollectionThread =
            std::thread{[this, socket = std::move(crashDumpSocket)]() mutable { CollectCrashDumps(std::move(socket)); }};
    }

    // Register a callback to detect if the utility VM exits unexpectedly.
    wsl::windows::common::hcs::RegisterCallback(m_system.get(), s_OnExit, this);
    signalEarlyTermination.release();

    // Start the utility VM.
    try
    {
        wsl::windows::common::hcs::StartComputeSystem(m_system.get(), json.c_str());
    }
    catch (...)
    {
        // Reset m_system so we don't try to wait for termination in the destructor, since the VM isn't even running.
        m_system.reset();
        throw;
    }

    // Add GPUs to the utility VM.
    if (m_vmConfig.EnableGpuSupport)
    {
        ExecutionContext context(Context::ConfigureGpu);

        hcs::ModifySettingRequest<hcs::GpuConfiguration> gpuRequest{};
        gpuRequest.ResourcePath = L"VirtualMachine/ComputeTopology/Gpu";
        gpuRequest.RequestType = hcs::ModifyRequestType::Update;
        gpuRequest.Settings.AssignmentMode = hcs::GpuAssignmentMode::Mirror;
        gpuRequest.Settings.AllowVendorExtension = true;
        if (IsDisableVgpuSettingsSupported())
        {
            gpuRequest.Settings.DisableGdiAcceleration = true;
            gpuRequest.Settings.DisablePresentation = true;
        }

        wsl::windows::common::hcs::ModifyComputeSystem(m_system.get(), wsl::shared::ToJsonW(gpuRequest).c_str());

        // Also add 9p shares for the library directories.
        // N.B. These are not hosted by the out-of-proc drvfs 9p server because the GPU shares
        //      should work even if drvfs is disabled.
        auto addShare = [&](PCWSTR name, PCWSTR path) {
            constexpr auto flags = (hcs::Plan9ShareFlags::ReadOnly | hcs::Plan9ShareFlags::AllowOptions);
            wsl::windows::common::hcs::AddPlan9Share(m_system.get(), name, name, path, LX_INIT_UTILITY_VM_PLAN9_PORT, flags);
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

    // Accept a connection from mini_init with a receive timeout so the service does not get stuck waiting for a response from the VM.
    m_miniInitChannel = wsl::shared::SocketChannel{AcceptConnection(m_vmConfig.KernelBootTimeout), "mini_init", m_terminatingEvent.get()};

    // Accept the connection from the Linux guest for notifications.
    m_notifyChannel = AcceptConnection(m_vmConfig.KernelBootTimeout);

    // Receive and parse the guest kernel version
    ReadGuestCapabilities();

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

            swapLun = AttachDiskLockHeld(m_vmConfig.SwapFilePath.c_str(), DiskType::VHD, MountFlags::None, {}, false, m_userToken.get());
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
    message->PageReportingOrder = m_coldDiscardShiftSize;
    message->MemoryReclaimMode = static_cast<LX_MINI_INIT_MEMORY_RECLAIM_MODE>(m_vmConfig.MemoryReclaim);
    message->EnableDebugShell = m_vmConfig.EnableDebugShell;
    message->EnableSafeMode = m_vmConfig.EnableSafeMode;
    message->EnableDnsTunneling = m_vmConfig.EnableDnsTunneling;
    message->DefaultKernel = m_defaultKernel;
    message->KernelModulesDeviceId = m_kernelModulesDeviceId;
    message.WriteString(message->HostnameOffset, wsl::windows::common::filesystem::GetLinuxHostName());
    message.WriteString(message->KernelModulesListOffset, m_vmConfig.KernelModulesList);
    message->DnsTunnelingIpAddress = m_vmConfig.DnsTunnelingIpAddress.value_or(0);

    m_miniInitChannel.SendMessage<LX_MINI_INIT_EARLY_CONFIG_MESSAGE>(message.Span());

    {
        ExecutionContext context(Context::ConfigureNetworking);

        // Accept the connection from the guest network service and create the channel.
        wsl::core::GnsChannel gnsChannel(AcceptConnection(m_vmConfig.KernelBootTimeout));

        // Create hvsocket connection for DNS tunneling if enabled.
        wil::unique_socket dnsTunnelingSocket;
        if (m_vmConfig.EnableDnsTunneling)
        {
            dnsTunnelingSocket = AcceptConnection(m_vmConfig.KernelBootTimeout);
        }

        // Record the start time of the networking engine initialization so the duration can be logged.
        const auto startTime = std::chrono::steady_clock::now();

        // For NAT networking, ensure the network can be created. If creating the network fails, fall back to
        // virtio proxy networking mode.
        wsl::windows::common::hcs::unique_hcn_network natNetwork;
        if (m_vmConfig.NetworkingMode == NetworkingMode::Nat)
        {
            natNetwork = wsl::core::NatNetworking::CreateNetwork(m_vmConfig);
            if (!natNetwork)
            {
                EMIT_USER_WARNING(wsl::shared::Localization::MessageNetworkInitializationFailedFallback2(
                    ToString(m_vmConfig.NetworkingMode), ToString(NetworkingMode::VirtioProxy)));

                m_vmConfig.NetworkingMode = NetworkingMode::VirtioProxy;
            }
        }

        // Create and initialize the networking engine.
        const auto result = wil::ResultFromException(WI_DIAGNOSTICS_INFO, [&] {
            if (m_vmConfig.NetworkingMode == NetworkingMode::Mirrored)
            {
                m_networkingEngine = std::make_unique<wsl::core::MirroredNetworking>(
                    m_system.get(), std::move(gnsChannel), m_vmConfig, m_runtimeId, std::move(dnsTunnelingSocket));
            }
            else if (m_vmConfig.NetworkingMode == NetworkingMode::Nat)
            {
                WI_ASSERT(natNetwork);

                m_networkingEngine = std::make_unique<wsl::core::NatNetworking>(
                    m_system.get(), std::move(natNetwork), std::move(gnsChannel), m_vmConfig, std::move(dnsTunnelingSocket));
            }
            else if (m_vmConfig.NetworkingMode == NetworkingMode::VirtioProxy)
            {
                m_networkingEngine = std::make_unique<wsl::core::VirtioNetworking>(
                    std::move(gnsChannel), m_vmConfig.EnableLocalhostRelay, m_guestDeviceManager, m_userToken);
            }
            else if (m_vmConfig.NetworkingMode == NetworkingMode::Bridged)
            {
                m_networkingEngine = std::make_unique<wsl::core::BridgedNetworking>(m_system.get(), m_vmConfig);
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
                EMIT_USER_WARNING(Localization::MessageVirtualMachinePlatformNotInstalled());
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

    if (m_system)
    {
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
                    wsl::windows::common::hcs::TerminateComputeSystem(m_system.get());
                }
                CATCH_LOG()
            }
        }

        m_vmExitEvent.wait(UTILITY_VM_TERMINATE_TIMEOUT);

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
            TraceLoggingValue(m_exitDetails.c_str(), "exitDetails"));
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

    // Close the handle to the VM. This will wait for any outstanding callbacks.
    m_system.reset();

    // This loops helps against a potential crash in build <= Windows 11 22H2.
    for (const auto& e : m_plan9Servers)
    {
        LOG_IF_FAILED(e.second->Teardown());
    }

    // Shutdown virtio device hosts.
    if (m_guestDeviceManager)
    {
        m_guestDeviceManager->Shutdown();
    }

    // Call RevokeVmAccess on each VHD that was added to the utility VM. This
    // ensures that the ACL on the VHD does not grow unbounded.
    std::for_each(m_attachedDisks.begin(), m_attachedDisks.end(), [&](const auto& Entry) {
        if ((Entry.first.Type == DiskType::PassThrough) && (WI_IsFlagSet(Entry.second.Flags, DiskStateFlags::Online)))
        {
            RestorePassthroughDiskState(Entry.first.Path.c_str());
        }

        if (WI_IsFlagSet(Entry.second.Flags, DiskStateFlags::AccessGranted))
        {
            try
            {
                wsl::windows::common::hcs::RevokeVmAccess(m_machineId.c_str(), Entry.first.Path.c_str());
            }
            CATCH_LOG()
        }
    });

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
    auto socket =
        wsl::windows::common::hvsocket::Accept(m_listenSocket.get(), m_vmConfig.KernelBootTimeout, m_terminatingEvent.get(), Location);
    if (ReceiveTimeout != 0)
    {
        THROW_LAST_ERROR_IF(setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&ReceiveTimeout, sizeof(ReceiveTimeout)) == SOCKET_ERROR);
    }

    return socket;
}

_Requires_lock_held_(m_guestDeviceLock)
void WslCoreVm::AddDrvFsShare(_In_ bool Admin, _In_ HANDLE UserToken)
{
    THROW_HR_IF(HCS_E_TERMINATED, !m_system);

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

    const auto virtiofsInitialized = Admin ? m_adminDrvfsToken.is_valid() : m_drvfsToken.is_valid();
    if (m_vmConfig.EnableVirtioFs && !virtiofsInitialized)
    {
        // Add virtiofs devices associating indices with paths from the fixed drive bitmap. These devices support
        // multiple mounts in the guest, so this only needs to be done once.
        // EX: drvfsC1 => C:\
        //     drvfsD2 => D:\
        //     drvfsaC3 => C:\ (elevated)
        auto fixedDrives = wsl::windows::common::filesystem::EnumerateFixedDrives(UserToken).first;
        while (fixedDrives != 0)
        {
            ULONG index;
            WI_VERIFY(_BitScanForward(&index, fixedDrives) != FALSE);
            const wchar_t fixedDrivePath[] = {gsl::narrow_cast<wchar_t>(L'A' + index), L':', L'\\', L'\0'};
            AddVirtioFsShare(Admin, fixedDrivePath, TEXT(LX_INIT_DEFAULT_PLAN9_MOUNT_OPTIONS), UserToken);
            fixedDrives ^= (1 << index);
        }
    }
}

bool WslCoreVm::IsDisableVgpuSettingsSupported() const
{
    // See if the Windows version has the required platform change.
    return ((wsl::windows::common::hcs::GetSchemaVersion() >= c_schemaVersionNickel) && (m_windowsVersion.BuildNumber >= 22545));
}

bool WslCoreVm::IsVirtioSerialConsoleSupported() const
{
    if (!m_vmConfig.EnableVirtio)
    {
        return false;
    }

    // See if the Windows version has the required platform change.
    //
    // N.B. If the package is running on a vibranium or iron build, then it means that lifted
    //      support is available, so virtio serial is available as well (since it was done in the same LCU).
    return m_windowsVersion.BuildNumber != WindowsBuildNumbers::Cobalt ||
           m_windowsVersion.UpdateBuildRevision >= VIRTIO_SERIAL_CONSOLE_COBALT_RELEASE_UBR;
}

bool WslCoreVm::IsVmemmSuffixSupported() const
{
    // See if the Windows version has the required platform change.
    return (
        (m_windowsVersion.BuildNumber >= VMMEM_SUFFIX_NICKEL_BUILD_NUMBER) ||
        ((m_windowsVersion.BuildNumber < NICKEL_BUILD_FLOOR) && (m_windowsVersion.BuildNumber >= VMEMM_SUFFIX_COBALT_REFRESH_BUILD_NUMBER)) ||
        ((m_windowsVersion.BuildNumber == WindowsBuildNumbers::Cobalt) &&
         (m_windowsVersion.UpdateBuildRevision >= VMMEM_SUFFIX_COBALT_RELEASE_UBR)));
}

_Requires_lock_held_(m_guestDeviceLock)
void WslCoreVm::AddPlan9Share(
    _In_ PCWSTR AccessName, _In_ PCWSTR Path, [[maybe_unused]] _In_ UINT32 Port, _In_ hcs::Plan9ShareFlags Flags, _In_ HANDLE UserToken, _In_opt_ PCWSTR VirtIoTag)
{
    bool addNewDevice = false;
    wil::com_ptr<IPlan9FileSystem> server;

    {
        auto revert = wil::impersonate_token(UserToken);

        // This is called from AddDrvFsShare, which is called from InitializeDrvFs, so m_guestDeviceLock is
        // already held.

        if (m_vmConfig.EnableVirtio9p)
        {
            server = m_guestDeviceManager->GetRemoteFileSystem(__uuidof(p9fs::Plan9FileSystem), VirtIoTag);
        }
        else
        {
            const auto existingServer = m_plan9Servers.find(Port);
            if (existingServer != m_plan9Servers.end())
            {
                server = existingServer->second;
            }
        }

        if (!server)
        {
            server = wsl::windows::common::wslutil::CreateComServerAsUser<p9fs::Plan9FileSystem, IPlan9FileSystem>(UserToken);
            if (m_vmConfig.EnableVirtio9p)
            {
                m_guestDeviceManager->AddRemoteFileSystem(__uuidof(p9fs::Plan9FileSystem), VirtIoTag, server);

                // Start with one device to handle the first mount request. After
                // each mount, the Plan9 file-system will request additional
                // devices via the IPlan9FileSystemHost::NotifyAllDevicesInUse
                // callback.
                addNewDevice = true;
            }
            else
            {
                THROW_IF_FAILED(server->Init(&m_runtimeId, Port));
                THROW_IF_FAILED(server->Resume());
                m_plan9Servers.insert(std::make_pair(Port, server));
            }
        }

        HRESULT result = server->AddSharePath(AccessName, Path, static_cast<UINT32>(Flags));
        if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        {
            result = S_OK;
        }

        THROW_IF_FAILED(result);
    }

    if (addNewDevice)
    {
        // This requires more privileges than the user may have, so impersonation is disabled.
        (void)m_guestDeviceManager->AddNewDevice(VIRTIO_PLAN9_DEVICE_ID, server, VirtIoTag);
    }
}

ULONG WslCoreVm::AttachDisk(_In_ PCWSTR Disk, _In_ DiskType Type, _In_ std::optional<ULONG> Lun, _In_ bool IsUserDisk, _In_ HANDLE UserToken)
{
    auto lock = m_lock.lock_exclusive();
    return AttachDiskLockHeld(Disk, Type, MountFlags::None, Lun, IsUserDisk, UserToken);
}

ULONG WslCoreVm::AttachDiskLockHeld(
    _In_ PCWSTR Disk, _In_ DiskType Type, _In_ MountFlags Flags, _In_ std::optional<ULONG> Lun, _In_ bool IsUserDisk, _In_opt_ HANDLE UserToken)
{
    ExecutionContext context(Context::MountDisk);

    Lun = ReserveLun(Lun);

    // Set a scope exit variable to perform cleanup if attaching the disk fails.
    DiskStateFlags diskFlags{};
    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        FreeLun(Lun.value());
        if (WI_IsFlagSet(diskFlags, DiskStateFlags::AccessGranted))
        {
            wsl::windows::common::hcs::RevokeVmAccess(m_machineId.c_str(), Disk);
        }

        if (WI_IsFlagSet(diskFlags, DiskStateFlags::Online))
        {
            const auto diskHandle = wsl::windows::common::disk::OpenDevice(Disk, GENERIC_READ | GENERIC_WRITE, m_vmConfig.MountDeviceTimeout);
            wsl::windows::common::disk::SetOnline(diskHandle.get(), false, m_vmConfig.MountDeviceTimeout);
        }
    });

    try
    {
        // Check if the disk is already attached.
        const auto found = m_attachedDisks.find({Type, Disk});

        if (Type == DiskType::PassThrough)
        {
            if (found != m_attachedDisks.end())
            {
                THROW_HR_WITH_USER_ERROR(WSL_E_DISK_ALREADY_ATTACHED, Localization::MessageDiskAlreadyAttached(Disk));
            }

            // Grant the VM access to the disk.
            GrantVmWorkerProcessAccessToDisk(Disk, UserToken);
            WI_SetFlag(diskFlags, DiskStateFlags::AccessGranted);

            // Set the disk online if needed.
            //
            // N.B. The disk handle must be closed prior to adding the disk to the VM.
            {
                const auto diskHandle =
                    wsl::windows::common::disk::OpenDevice(Disk, GENERIC_READ | GENERIC_WRITE, m_vmConfig.MountDeviceTimeout);
                if (wsl::windows::common::disk::IsDiskOnline(diskHandle.get()))
                {
                    wsl::windows::common::disk::SetOnline(diskHandle.get(), false, m_vmConfig.MountDeviceTimeout);
                    WI_SetFlag(diskFlags, DiskStateFlags::Online);
                }
            }

            // Add the disk to the VM.
            wsl::shared::retry::RetryWithTimeout<void>(
                std::bind(wsl::windows::common::hcs::AddPassThroughDisk, m_system.get(), Disk, Lun.value()),
                wsl::windows::common::disk::c_diskOperationRetry,
                std::chrono::milliseconds(m_vmConfig.MountDeviceTimeout),
                []() { return wil::ResultFromCaughtException() == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION); });
        }
        else
        {
            if (found != m_attachedDisks.end())
            {
                // Prevent user from launching a distro vhd after manually mounting it; otherwise, return the LUN of the mounted disk.
                THROW_HR_IF(WSL_E_USER_VHD_ALREADY_ATTACHED, found->first.User);

                return found->second.Lun;
            }

            auto grantDiskAccess = [&]() {
                auto runAsUser = wil::impersonate_token(UserToken);
                wsl::windows::common::hcs::GrantVmAccess(m_machineId.c_str(), Disk);
                WI_SetFlag(diskFlags, DiskStateFlags::AccessGranted);
            };

            // Grant the VM access to the disk.
            if (WI_IsFlagClear(Flags, MountFlags::ReadOnly))
            {
                grantDiskAccess();
            }

            auto result = wil::ResultFromException([&]() {
                wsl::windows::common::hcs::AddVhd(m_system.get(), Disk, Lun.value(), WI_IsFlagSet(Flags, MountFlags::ReadOnly));
            });

            if (result == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) && WI_IsFlagClear(diskFlags, DiskStateFlags::AccessGranted))
            {
                grantDiskAccess();
                wsl::windows::common::hcs::AddVhd(m_system.get(), Disk, Lun.value(), WI_IsFlagSet(Flags, MountFlags::ReadOnly));
            }
            else
            {
                THROW_IF_FAILED(result);
            }
        }
    }
    catch (...)
    {
        const auto result = wil::ResultFromCaughtException();
        THROW_HR_WITH_USER_ERROR(
            result, Localization::MessageFailedToAttachDisk(Disk, wsl::windows::common::wslutil::GetSystemErrorString(result)));
    }

    m_attachedDisks.emplace(AttachedDisk{Type, Disk, IsUserDisk}, DiskState{Lun.value(), {}, diskFlags});
    cleanup.release();

    return Lun.value();
}

void WslCoreVm::CollectCrashDumps(wil::unique_socket&& listenSocket) const
{
    wsl::windows::common::wslutil::SetThreadDescription(L"CrashDumpCollection");

    while (!m_terminatingEvent.is_signaled())
    {
        try
        {
            auto socket = wsl::windows::common::hvsocket::Accept(listenSocket.get(), INFINITE, m_terminatingEvent.get());

            DWORD receiveTimeout = m_vmConfig.KernelBootTimeout;
            THROW_LAST_ERROR_IF(
                setsockopt(listenSocket.get(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&receiveTimeout, sizeof(receiveTimeout)) == SOCKET_ERROR);

            auto channel = wsl::shared::SocketChannel{std::move(socket), "crash_dump", m_terminatingEvent.get()};

            const auto& message = channel.ReceiveMessage<LX_PROCESS_CRASH>();
            const char* process = reinterpret_cast<const char*>(&message.Buffer);

            constexpr auto dumpExtension = ".dmp";
            constexpr auto dumpPrefix = "wsl-crash";

            auto filename = std::format("{}-{}-{}-{}-{}{}", dumpPrefix, message.Timestamp, message.Pid, process, message.Signal, dumpExtension);

            std::replace_if(filename.begin(), filename.end(), [](auto e) { return !std::isalnum(e) && e != '.' && e != '-'; }, '_');

            auto fullPath = m_vmConfig.CrashDumpFolder / filename;

            // Log telemetry when there is a crash within the WSL VM
            WSL_LOG_TELEMETRY(
                "LinuxCrash",
                PDT_ProductAndServicePerformance,
                TraceLoggingValue(fullPath.c_str(), "FullPath"),
                TraceLoggingValue(message.Pid, "Pid"),
                TraceLoggingValue(message.Signal, "Signal"),
                TraceLoggingValue(process, "process"));

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

            channel.SendResultMessage<std::int32_t>(0);

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
    const auto lun = AttachDiskLockHeld(Configuration.VhdFilePath.c_str(), DiskType::VHD, MountFlags::None, {}, false, m_userToken.get());

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
    message->MountDeviceType = LxMiniInitMountDeviceTypeLun;
    message->DeviceId = lun;
    message->Flags = flags;
    message.WriteString(message->FsTypeOffset, "ext4");
    message.WriteString(message->MountOptionsOffset, "discard,errors=remount-ro,data=ordered");
    message.WriteString(message->VmIdOffset, m_machineId);
    message.WriteString(message->DistributionNameOffset, Configuration.Name);
    message.WriteString(message->SharedMemoryRootOffset, sharedMemoryRoot);
    message.WriteString(message->InstallPathOffset, installPath);
    message.WriteString(message->UserProfileOffset, userProfile);
    m_miniInitChannel.SendMessage<LX_MINI_INIT_MESSAGE>(message.Span());

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
    auto initSocket = AcceptConnection(ReceiveTimeout);

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
        std::bind(s_InitializeDrvFs, this, std::placeholders::_1),
        featureFlags,
        m_vmConfig.DistributionStartTimeout,
        m_vmConfig.InstanceIdleTimeout,
        ConnectPort);

    WI_ASSERT(!initSocket && !systemDistroSocket);

    return instance;
}

wil::unique_socket WslCoreVm::CreateListeningSocket() const
{
    return wsl::windows::common::hvsocket::Listen(m_runtimeId, 0);
}

std::pair<int, LX_MINI_MOUNT_STEP> WslCoreVm::DetachDisk(_In_opt_ PCWSTR Disk)
{
    bool deleted = !ARGUMENT_PRESENT(Disk);
    std::vector<AttachedDisk> selectedDisks;

    auto diskMatches = [TargetPath = Disk](const AttachedDisk& disk) {
        if (!disk.User)
        {
            // Only user mounted disks can be detached
            return false;
        }

        if (disk.Type == DiskType::VHD)
        {
            // N.B. std::filesystem::equivalent can throw if the path is malformed so use the noexcept variant.
            std::error_code error{};
            return TargetPath == nullptr || std::filesystem::equivalent(disk.Path, TargetPath, error);
        }
        else if (disk.Type == DiskType::PassThrough)
        {
            return TargetPath == nullptr || wsl::windows::common::string::IsPathComponentEqual(disk.Path, TargetPath);
        }

        return false;
    };

    auto lock = m_lock.lock_exclusive();
    for (auto it = m_attachedDisks.begin(); it != m_attachedDisks.end();)
    {
        if (diskMatches(it->first))
        {
            // Unmount any mounted volumes inside the utility VM.
            const auto result = UnmountDisk(it->first, it->second);
            if (result.first != 0)
            {
                return result;
            }

            // Detach the disk from the VM.
            wsl::windows::common::hcs::RemoveScsiDisk(m_system.get(), it->second.Lun);
            if (WI_VERIFY(WI_IsFlagSet(it->second.Flags, DiskStateFlags::AccessGranted)))
            {
                wsl::windows::common::hcs::RevokeVmAccess(m_machineId.c_str(), it->first.Path.c_str());
            }

            FreeLun(it->second.Lun);

            // If the disk was online before being attached, revert to that state.
            if (WI_IsFlagSet(it->second.Flags, DiskStateFlags::Online))
            {
                RestorePassthroughDiskState(it->first.Path.c_str());
            }

            deleted = true;
            it = m_attachedDisks.erase(it);
        }
        else
        {
            ++it;
        }
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
    const auto search = m_attachedDisks.find({DiskType::VHD, VhdPath});
    if (search != m_attachedDisks.end())
    {
        EJECT_VHD_MESSAGE message;
        message.Header.MessageSize = sizeof(message);
        message.Header.MessageType = LxMiniInitMessageEjectVhd;
        message.Lun = search->second.Lun;
        const auto& result = m_miniInitChannel.Transaction(message);
        LOG_HR_IF_MSG(E_UNEXPECTED, result.Result != 0, "VHD eject failed: %u", result.Result);

        // Impersonate the session manager and remove the vhd.
        {
            auto runAsSelf = wil::run_as_self();
            wsl::windows::common::hcs::RemoveScsiDisk(m_system.get(), search->second.Lun);
            if (WI_IsFlagSet(search->second.Flags, DiskStateFlags::AccessGranted))
            {
                wsl::windows::common::hcs::RevokeVmAccess(m_machineId.c_str(), VhdPath);
            }
        }

        m_attachedDisks.erase(search);
        FreeLun(message.Lun);
    }
}

_Requires_lock_held_(m_guestDeviceLock)
std::optional<WslCoreVm::VirtioFsShare> WslCoreVm::FindVirtioFsShare(_In_ PCWSTR tag, _In_ std::optional<bool> Admin) const
{
    for (const auto& share : m_virtioFsShares)
    {
        if ((share.second == tag) && (!Admin.has_value() || Admin.value() == share.first.Admin))
        {
            return share.first;
        }
    }

    return {};
}

void WslCoreVm::FreeLun(_In_ ULONG lun)
{
    WI_ASSERT(m_lunBitmap[lun]);
    m_lunBitmap.set(lun, false);
}

std::wstring WslCoreVm::GenerateConfigJson()
{
    hcs::ComputeSystem systemSettings{};
    systemSettings.Owner = wsl::windows::common::wslutil::c_vmOwner;
    systemSettings.ShouldTerminateOnLastHandleClosed = true;
    systemSettings.SchemaVersion.Major = 2;
    systemSettings.SchemaVersion.Minor = 3;
    hcs::VirtualMachine vmSettings{};
    vmSettings.StopOnReset = true;
    vmSettings.Chipset.UseUtc = true;

    // Ensure the 2MB granularity enforced by HCS.
    vmSettings.ComputeTopology.Memory.SizeInMB = ((m_vmConfig.MemorySizeBytes / _1MB) & ~0x1);
    vmSettings.ComputeTopology.Memory.AllowOvercommit = true;
    vmSettings.ComputeTopology.Memory.EnableDeferredCommit = true;
    vmSettings.ComputeTopology.Memory.EnableColdDiscardHint = true;

    // Configure backing page size, fault cluster shift size, and cold discard hint size to favor density (lower vmmem usage).
    //
    // N.B. Cold discard hint size should be a multiple of the fault cluster shift size.
    //
    // N.B. This is only done on builds that have the fix for the VID deadlock on partition teardown.
    if ((m_windowsVersion.BuildNumber >= WindowsBuildNumbers::Germanium) ||
        (m_windowsVersion.BuildNumber >= WindowsBuildNumbers::Cobalt && m_windowsVersion.UpdateBuildRevision >= 2360) ||
        (m_windowsVersion.BuildNumber >= WindowsBuildNumbers::Iron && m_windowsVersion.UpdateBuildRevision >= 1970) ||
        (m_windowsVersion.BuildNumber >= WindowsBuildNumbers::Vibranium_22H2 && m_windowsVersion.UpdateBuildRevision >= 3393))
    {
        vmSettings.ComputeTopology.Memory.BackingPageSize = hcs::MemoryBackingPageSize::Small;
        vmSettings.ComputeTopology.Memory.FaultClusterSizeShift = 4;          // 64k
        vmSettings.ComputeTopology.Memory.DirectMapFaultClusterSizeShift = 4; // 64k
        m_coldDiscardShiftSize = 5;                                           // 128k
    }
    else
    {
        m_coldDiscardShiftSize = 9; // 2MB
    }

    // May need more MMIO than the default 16GB. WSL uses a vpci device per Plan9 share, WSLg adds a GPU device,
    // and a pmem device, and each shared memory virtiofs device needs more than 8GB of MMIO.
    SafeInt<INT64> highMmioGapInMB = DEFAULT_HIGH_MMIO_GAP_IN_MB;

    // Add additional MMIO space for the system distro and WSLg.
    bool privateSystemDistro = !m_vmConfig.SystemDistroPath.empty();
    if (!privateSystemDistro)
    {
#ifdef WSL_SYSTEM_DISTRO_PATH

        m_vmConfig.SystemDistroPath = TEXT(WSL_SYSTEM_DISTRO_PATH);
        privateSystemDistro = true;

#else

        m_systemDistroDeviceType = LxMiniInitMountDeviceTypeLun;
        m_vmConfig.SystemDistroPath = (m_installPath / L"system.vhd").wstring();
        WI_ASSERT(wsl::windows::common::filesystem::FileExists(m_vmConfig.SystemDistroPath.c_str()));

#endif
    }

    // Ensure the system distro exists and ends with a img or vhd file extension.
    if (privateSystemDistro)
    {
        if (wsl::windows::common::string::IsPathComponentEqual(m_vmConfig.SystemDistroPath.extension().native(), L".img"))
        {
            m_systemDistroDeviceType = LxMiniInitMountDeviceTypePmem;
        }
        else if (wsl::windows::common::string::IsPathComponentEqual(m_vmConfig.SystemDistroPath.extension().native(), L".vhd"))
        {
            m_systemDistroDeviceType = LxMiniInitMountDeviceTypeLun;
        }

        THROW_HR_IF(
            WSL_E_CUSTOM_SYSTEM_DISTRO_ERROR,
            (m_systemDistroDeviceType == LxMiniInitMountDeviceTypeInvalid) ||
                (!wsl::windows::common::filesystem::FileExists(m_vmConfig.SystemDistroPath.c_str())));
    }

    // Add MMIO space for the WSLg virtio shared memory device.
    if (m_vmConfig.EnableGuiApps && m_vmConfig.EnableVirtio)
    {
        highMmioGapInMB += WSLG_SHARED_MEMORY_SIZE_MB + EXTRA_MMIO_SIZE_PER_VIRTIOFS_DEVICE_IN_MB;
    }

    // If using pmem for the system distro, add MMIO space for the device.
    if (m_systemDistroDeviceType == LxMiniInitMountDeviceTypePmem)
    {
        highMmioGapInMB += RequiredExtraMmioSpaceForPmemFileInMb(m_vmConfig.SystemDistroPath.c_str());
    }

    // Log telemetry to measure system distro usage.
    WSL_LOG(
        "InitializeSystemDistro",
        TraceLoggingValue(static_cast<INT64>(highMmioGapInMB), "highMmioGapInMB"),
        TraceLoggingValue(privateSystemDistro, "privateSystemDistro"),
        TraceLoggingValue(static_cast<DWORD>(m_systemDistroDeviceType), "systemDistroDeviceType"),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO));

    vmSettings.ComputeTopology.Memory.HighMmioGapInMB = highMmioGapInMB;

    // The guest may only be able to access 36-bits of address space (minimum supported), so shift the high MMIO base
    // down such that all addresses are accessible. The default starting point is 16G below the maximum 36-bit address,
    // so for guests that support larger address spaces, the default base should suffice.
    vmSettings.ComputeTopology.Memory.HighMmioBaseInMB = MAX_36_BIT_PAGE_IN_MB - highMmioGapInMB;

    // Configure the number of processors.
    vmSettings.ComputeTopology.Processor.Count = m_vmConfig.ProcessorCount;

    // Set the vmmem suffix which will change the process name in task manager.
    if (IsVmemmSuffixSupported())
    {
        vmSettings.ComputeTopology.Memory.HostingProcessNameSuffix = wsl::windows::common::wslutil::c_vmOwner;
    }

    // If nested virtualization was requested, ensure the platform supports it.
    //
    // N.B. This is done because arm64 and some older amd64 processors do not support nested virtualization.
    //      Nested virtualization not supported on Windows 10.
    if (m_vmConfig.EnableNestedVirtualization)
    {
        try
        {
            if (wsl::windows::common::helpers::IsWindows11OrAbove())
            {
                const auto& processorFeatures = wsl::windows::common::hcs::GetProcessorFeatures();
                auto feature = std::find(processorFeatures.begin(), processorFeatures.end(), "NestedVirt");
                m_vmConfig.EnableNestedVirtualization = (feature != processorFeatures.end());
            }
            else
            {
                m_vmConfig.EnableNestedVirtualization = false;
            }

            vmSettings.ComputeTopology.Processor.ExposeVirtualizationExtensions = m_vmConfig.EnableNestedVirtualization;
            if (!m_vmConfig.EnableNestedVirtualization)
            {
                EMIT_USER_WARNING(wsl::shared::Localization::MessageNestedVirtualizationNotSupported());
            }
        }
        CATCH_LOG()
    }

#ifdef _AMD64_

    // Enable hardware performance counters if they are supported.
    if (m_vmConfig.EnableHardwarePerformanceCounters)
    {
        HV_X64_HYPERVISOR_HARDWARE_FEATURES hardwareFeatures{};
        __cpuid(reinterpret_cast<int*>(&hardwareFeatures), HvCpuIdFunctionMsHvHardwareFeatures);
        vmSettings.ComputeTopology.Processor.EnablePerfmonPmu = hardwareFeatures.ChildPerfmonPmuSupported != 0;
        vmSettings.ComputeTopology.Processor.EnablePerfmonLbr = hardwareFeatures.ChildPerfmonLbrSupported != 0;
    }

#endif

    // Initialize kernel command line.
    std::wstring kernelCmdLine = L"initrd=\\" LXSS_VM_MODE_INITRD_NAME L" " TEXT(WSL_ROOT_INIT_ENV) L"=1 panic=-1";

    // Set number of processors.
    kernelCmdLine += std::format(L" nr_cpus={}", m_vmConfig.ProcessorCount);

    // Enable timesync workaround to sync on resume from sleep in modern standby.
    kernelCmdLine += L" hv_utils.timesync_implicit=1";

    // If using virtio-9p, enable SWIOTLB as a perf optimization (will cause VM to consume 64MB more memory).
    if (m_vmConfig.EnableVirtio9p)
    {
        kernelCmdLine += L" swiotlb=force";
    }

    if (IsVirtioSerialConsoleSupported())
    {
        vmSettings.Devices.VirtioSerial.emplace();
    }

    if (m_dmesgCollector)
    {
        if (m_vmConfig.EnableEarlyBootLogging)
        {
            // Capture using the very slow legacy serial port up until the point that the virtio device is started.
            if constexpr (!wsl::shared::Arm64)
            {
                kernelCmdLine += L" earlycon=uart8250,io,0x3f8,115200";
            }
            else
            {
                kernelCmdLine += L" earlycon=pl011,0xeffec000,115200";
            }

            vmSettings.Devices.ComPorts["0"] = hcs::ComPort{m_dmesgCollector->EarlyConsoleName()};
        }

        // The primary "console" will be a virtio serial device.
        kernelCmdLine += L" console=hvc0 debug";
        hcs::VirtioSerialPort virtioPort{};
        virtioPort.Name = L"hvc0";
        virtioPort.NamedPipe = m_dmesgCollector->VirtioConsoleName();
        virtioPort.ConsoleSupport = true;
        vmSettings.Devices.VirtioSerial->Ports["0"] = std::move(virtioPort);
    }
    else if (m_vmConfig.EnableDebugConsole)
    {
        // If a debug console was requested, add required kernel command line options.
        if constexpr (!wsl::shared::Arm64)
        {
            kernelCmdLine += L" console=ttyS0,115200 debug";
        }
        else
        {
            kernelCmdLine += L" console=ttyAMA0 debug";
        }
    }

    //
    // N.B. The ordering of these devices is important because it determines the order they show up as
    //      /dev/hvc devices in the guest.
    //

    if (m_gnsTelemetryLogger)
    {
        hcs::VirtioSerialPort virtioPort;
        virtioPort.Name = TEXT(LX_INIT_HVC_TELEMETRY);
        virtioPort.NamedPipe = m_gnsTelemetryLogger->GetPipeName();
        virtioPort.ConsoleSupport = true;
        vmSettings.Devices.VirtioSerial->Ports["1"] = std::move(virtioPort);
    }

    if (!m_debugShellPipe.empty())
    {
        hcs::VirtioSerialPort virtioPort;
        virtioPort.Name = TEXT(LX_INIT_HVC_DEBUG_SHELL);
        virtioPort.NamedPipe = m_debugShellPipe;
        virtioPort.ConsoleSupport = true;
        vmSettings.Devices.VirtioSerial->Ports["2"] = std::move(virtioPort);
    }

    // Ensure that virtio serial devices have unique names.
    if constexpr (wsl::shared::Debug)
    {
        if (vmSettings.Devices.VirtioSerial)
        {
            std::set<std::wstring_view> uniqueNames;
            for (const auto& device : vmSettings.Devices.VirtioSerial->Ports)
            {
                uniqueNames.emplace(device.second.Name);
            }

            WI_ASSERT_MSG(
                uniqueNames.size() == vmSettings.Devices.VirtioSerial->Ports.size(), "Serial device names must be unique.");
        }
    }

    // If a kernel debugger was requested, add required kernel command line options and
    // generate the name of the pipe.
    if (m_vmConfig.KernelDebugPort != 0)
    {
        PCWSTR debugDeviceName = nullptr;
        if constexpr (wsl::shared::Arm64)
        {
            debugDeviceName = L"ttyAMA1";
        }
        else
        {
            debugDeviceName = L"ttyS1";
        }

        kernelCmdLine += std::format(L" pty.legacy_count=2 kgdboc={},115200", debugDeviceName);

        m_comPipe1 = wsl::windows::common::helpers::GetUniquePipeName();
        wsl::windows::common::helpers::LaunchKdRelay(
            m_comPipe1.c_str(), m_restrictedToken.get(), m_vmConfig.KernelDebugPort, m_terminatingEvent.get(), !m_vmConfig.EnableTelemetry);
    }
    else
    {
        kernelCmdLine += L" pty.legacy_count=0";
    }

    if (!m_comPipe0.empty() && (!m_dmesgCollector || !m_vmConfig.EnableEarlyBootLogging))
    {
        vmSettings.Devices.ComPorts["0"] = hcs::ComPort{m_comPipe0};
    }

    if (!m_comPipe1.empty())
    {
        vmSettings.Devices.ComPorts["1"] = hcs::ComPort{m_comPipe1};
    }

    if (m_vmConfig.MaxCrashDumpCount >= 0)
    {
        kernelCmdLine += L" " WSL_ENABLE_CRASH_DUMP_ENV L"=1";
    }

    // Add user-specified kernel command line options at the end.
    if (!m_vmConfig.KernelCommandLine.empty())
    {
        kernelCmdLine += L" ";
        kernelCmdLine += m_vmConfig.KernelCommandLine;
    }

    // Set up boot params.
    //
    // N.B. Linux kernel direct boot is not yet supported on ARM64.
    if constexpr (!wsl::shared::Arm64)
    {
        auto linuxKernelDirect = hcs::LinuxKernelDirect{};
        linuxKernelDirect.KernelFilePath = m_vmConfig.KernelPath.c_str();
        linuxKernelDirect.InitRdPath = (m_rootFsPath / LXSS_VM_MODE_INITRD_NAME).c_str();
        linuxKernelDirect.KernelCmdLine = kernelCmdLine;
        vmSettings.Chipset.LinuxKernelDirect = std::move(linuxKernelDirect);
    }
    else
    {
        auto bootThis = hcs::UefiBootEntry{};
        bootThis.DeviceType = hcs::UefiBootDevice::VmbFs;
        bootThis.VmbFsRootPath = m_rootFsPath.c_str();
        bootThis.DevicePath = L"\\" LXSS_VM_MODE_KERNEL_NAME;
        bootThis.OptionalData = kernelCmdLine;
        hcs::Uefi uefiSettings{};
        uefiSettings.BootThis = std::move(bootThis);
        vmSettings.Chipset.Uefi = std::move(uefiSettings);
    }

    // Initialize SCSI devices.
    hcs::Scsi scsiController{};
    auto attachDisk = [&](PCWSTR path) {
        auto lun = ReserveLun();
        hcs::Attachment disk{};
        disk.Type = hcs::AttachmentType::VirtualDisk;
        disk.Path = path;
        disk.ReadOnly = true;
        disk.SupportCompressedVolumes = true;
        disk.AlwaysAllowSparseFiles = true;
        disk.SupportEncryptedFiles = true;
        scsiController.Attachments[std::to_string(lun)] = std::move(disk);
        m_attachedDisks.emplace(AttachedDisk{DiskType::VHD, path, false}, DiskState{lun, {}, {}});
        return lun;
    };

    if (m_systemDistroDeviceType == LxMiniInitMountDeviceTypeLun)
    {
        m_systemDistroDeviceId = attachDisk(m_vmConfig.SystemDistroPath.c_str());
    }

    if (!m_vmConfig.KernelModulesPath.empty())
    {
        m_kernelModulesDeviceId = attachDisk(m_vmConfig.KernelModulesPath.c_str());
    }

    vmSettings.Devices.Scsi["0"] = std::move(scsiController);

    // Construct a security descriptor that allows system and the current user.
    wil::unique_hlocal_string userSidString;
    THROW_LAST_ERROR_IF(!ConvertSidToStringSidW(&m_userSid.Sid, &userSidString));

    std::wstring securityDescriptor{L"D:P(A;;FA;;;SY)(A;;FA;;;"};
    securityDescriptor += userSidString.get();
    securityDescriptor += L")";
    hcs::HvSocket hvSocketConfig{};
    hvSocketConfig.HvSocketConfig.DefaultBindSecurityDescriptor = securityDescriptor;
    hvSocketConfig.HvSocketConfig.DefaultConnectSecurityDescriptor = securityDescriptor;
    vmSettings.Devices.HvSocket = std::move(hvSocketConfig);

    // N.B. Plan9 device is always added during serialization

    systemSettings.VirtualMachine = std::move(vmSettings);
    return wsl::shared::ToJsonW(systemSettings);
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

void WslCoreVm::GrantVmWorkerProcessAccessToDisk(_In_ PCWSTR Disk, _In_opt_ HANDLE UserToken) const
{
    if (ARGUMENT_PRESENT(UserToken))
    {
        // Impersonating the user doesn't let us access a block device,
        // check for an elevated token instead.
        THROW_HR_IF(WSL_E_ELEVATION_NEEDED_TO_MOUNT_DISK, ((!wsl::windows::common::security::IsTokenElevated(UserToken))));
    }

    wsl::windows::common::hcs::GrantVmAccess(m_machineId.c_str(), Disk);
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
                // Use the appropriate virtiofs class ID based on m_userToken elevation.
                const bool admin = wsl::windows::common::security::IsTokenElevated(m_userToken.get());
                const GUID classId = admin ? VIRTIO_FS_ADMIN_CLASS_ID : VIRTIO_FS_CLASS_ID;
                m_guestDeviceManager->AddSharedMemoryDevice(classId, L"wslg", L"wslg", WSLG_SHARED_MEMORY_SIZE_MB, m_userToken.get());
                m_sharedMemoryRoot = std::format(L"WSL\\{}\\wslg", m_machineId);
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
    m_miniInitChannel.SendMessage<LX_MINI_INIT_CONFIG_MESSAGE>(message.Span());

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
            wsl::windows::common::helpers::LaunchPortRelay(socket.get(), m_runtimeId, m_restrictedToken.get(), !m_vmConfig.EnableTelemetry);
        });

        if (FAILED(result))
        {
            const auto errorString = wsl::windows::common::wslutil::GetSystemErrorString(result);
            EMIT_USER_WARNING(wsl::shared::Localization::MessageLocalhostRelayFailed(errorString));
        }
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
    WI_ASSERT(m_vmConfig.NetworkingMode == NetworkingMode::Nat || m_vmConfig.NetworkingMode == NetworkingMode::Mirrored);

    return SUCCEEDED_LOG(wsl::core::networking::DnsResolver::LoadDnsResolverMethods());
}

bool WslCoreVm::IsVhdAttached(_In_ PCWSTR VhdPath)
{
    auto lock = m_lock.lock_exclusive();
    return m_attachedDisks.contains({DiskType::VHD, VhdPath});
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
    const auto it = m_attachedDisks.find({MountDiskType, Disk});
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), (it == m_attachedDisks.end()));
    THROW_HR_IF(WSL_E_DISK_ALREADY_MOUNTED, it->second.Mounts.find(PartitionIndex) != it->second.Mounts.end());

    // Get the name for the mountpoint
    auto targetName = s_GetMountTargetName(Disk, Name, PartitionIndex);
    auto targetNameWide = wsl::shared::string::MultiByteToWide(targetName);
    // For each attachedDisk pair
    const auto nameCollision = std::any_of(m_attachedDisks.begin(), m_attachedDisks.end(), [&](const auto& diskEntry) {
        // Check if the targetName matches the name of any Mount already present
        return (std::any_of(diskEntry.second.Mounts.begin(), diskEntry.second.Mounts.end(), [&](const auto& mountEntry) {
            return wsl::shared::string::IsEqual(mountEntry.second.Name, targetNameWide, false);
        }));
    });

    // Throw error if the specified name was already used
    THROW_HR_IF(WSL_E_VM_MODE_MOUNT_NAME_ALREADY_EXISTS, nameCollision);

    wsl::shared::MessageWriter<LX_MINI_INIT_MOUNT_MESSAGE> message(LxMiniInitMessageMount);
    message->PartitionIndex = PartitionIndex;
    message->ScsiLun = it->second.Lun;
    message.WriteString(message->TypeOffset, Type);
    message.WriteString(message->TargetNameOffset, targetName);
    message.WriteString(message->OptionsOffset, Options);

    // Send the message.
    m_miniInitChannel.SendMessage<LX_MINI_INIT_MOUNT_MESSAGE>(message.Span());

    // Accept a connection from mini_init
    wsl::shared::SocketChannel channel{AcceptConnection(m_vmConfig.KernelBootTimeout), "MountResult", m_terminatingEvent.get()};

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

        it->second.Mounts.emplace(PartitionIndex, std::move(mount));
    }

    return {std::move(targetName), mountResult, step};
}

wil::unique_socket WslCoreVm::CreateRootNamespaceProcess(_In_ LPCSTR Path, _In_ LPCSTR* Arguments)
{
    auto lock = m_lock.lock_exclusive();

    return LxssCreateProcess::CreateLinuxProcess(
        Path, Arguments, m_runtimeId, m_miniInitChannel, m_terminatingEvent.get(), m_vmConfig.DistributionStartTimeout);
}

void WslCoreVm::MountRootNamespaceFolder(_In_ LPCWSTR HostPath, _In_ LPCWSTR GuestPath, _In_ bool ReadOnly, _In_ LPCWSTR Name)
{
    auto lock = m_lock.lock_exclusive();

    const auto flags = (ReadOnly ? hcs::Plan9ShareFlags::ReadOnly : hcs::Plan9ShareFlags::None) | hcs::Plan9ShareFlags::AllowOptions;
    wsl::windows::common::hcs::AddPlan9Share(m_system.get(), Name, Name, HostPath, LX_INIT_UTILITY_VM_PLAN9_PORT, flags);

    wsl::shared::MessageWriter<LX_MINI_INIT_MOUNT_FOLDER_MESSAGE> message(LxMiniInitMountFolder);
    message.WriteString(message->PathIndex, GuestPath);
    message.WriteString(message->NameIndex, Name);
    message->ReadOnly = ReadOnly;

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
    hcs::Plan9ShareFlags flags{};

    WI_SetFlagIf(flags, hcs::Plan9ShareFlags::ReadOnly, ReadOnly);

    // Serialize calls to mount pmem devices to the VM. Some quick background on why we do this.
    // The problem stems from the fact that our caller needs to know the dev path where the pmem
    // device will be mounted (i.e. /dev/pmem0). We could dynamically discover the device path and
    // return that to our caller. However, some callers statically declare the dev paths in their
    // fstabs. Therefore, we must wait for each device to finish initializing before allowing the
    // next to proceed, so that they appear in the expected predefined order.
    //
    // Ideally callers wouldn't rely on the dev path, and would setup their fstabs using names. If
    // callers are ever updated, we could update this code to allow pmem devices to be added in
    // parallel and dynamically discover their dev path (which we would then use if the caller
    // asked us to mount the pmem device, instead of them doing it in their fstabs). To dynamically
    // discover the dev path, we'd have to poll /sys/class/block. Eventually a path such as
    // /sys/class/block/pmemX will appear. Once it appears, /sys/class/block/pmemX/device will be
    // a symlink that points to a path like:
    // /sys/devices/LNXSYSTM:00/LNXSYBUS:00/ACPI0004:00/VMBUS:00/<GUID>/pcicceb:00//cceb:00:00.0/virtio1/ndbus0/region0/namespace0.0/block/pmem0
    // Notice the GUID in the middle of that path. That GUID is the instance ID, which is randomly
    // generated by AddGuestDevice. So once we find a path with the instance ID, we know that
    // eventually /dev/pmemX will appear in the guest.
    auto persistentMemoryLock = m_persistentMemoryLock.lock_exclusive();

    // Add the pmem device to the VM.
    // N.B. If this succeeds, technically we'd need to remove the device if we later encounter any
    //      failures. Otherwise, we'd potentially leave the VM in a torn state. However, HCS
    //      doesn't currently support this. For now, we rely on the fact that all pmem devices are
    //      added as part of VM creation and therefore any failure will result in VM termination
    //      (in which case there's no need to remove the device).
    {
        (void)m_guestDeviceManager->AddGuestDevice(
            VIRTIO_PMEM_DEVICE_ID, VIRTIO_PMEM_CLASS_ID, L"", nullptr, FilePath, static_cast<UINT32>(flags), m_userToken.get());
    }

    // Wait for the pmem device to appear in the VM at /dev/pmemX. Guess the value of X given the
    // number of pmem devices that have been exposed to the VM. See above for more details why.
    // N.B. If hot remove of pmem devices is ever added, this logic will need to be updated.
    //      Similarly, if nvdimm devices are ever passed through to the VM, this logic will need
    //      to be updated.
    const ULONG persistentMemoryId = m_nextPersistentMemoryId;
    WaitForPmemDeviceInVm(persistentMemoryId);

    // The pmem device was successfully found in the VM. Increment the next expected pmem device ID.
    m_nextPersistentMemoryId += 1;

    return persistentMemoryId;
}

void WslCoreVm::WaitForPmemDeviceInVm(_In_ ULONG PmemId)
{
    // Construct the mini_init message.
    LX_MINI_INIT_WAIT_FOR_PMEM_DEVICE_MESSAGE message;
    message.Header.MessageType = LxMiniInitMessageWaitForPmemDevice;
    message.Header.MessageSize = sizeof(message);
    message.PmemId = PmemId;

    // Send the message to mini_init.
    wsl::shared::SocketChannel channel;
    {
        auto lock = m_lock.lock_exclusive();

        m_miniInitChannel.SendMessage(message);
        channel = {
            AcceptConnection(m_vmConfig.KernelBootTimeout),
            "WaitForPmem",
            m_terminatingEvent.get(),
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
std::wstring WslCoreVm::AddVirtioFsShare(_In_ bool Admin, _In_ PCWSTR Path, _In_ PCWSTR Options, _In_opt_ HANDLE UserToken)
{
    WI_ASSERT(m_vmConfig.EnableVirtioFs && wsl::shared::string::IsDriveRoot(wsl::shared::string::WideToMultiByte(Path)));

    if (!ARGUMENT_PRESENT(UserToken))
    {
        UserToken = Admin ? m_adminDrvfsToken.get() : m_drvfsToken.get();
        THROW_HR_IF_MSG(E_UNEXPECTED, !UserToken, "UserToken not set for supplied context (Admin = %d)", Admin);
    }

    WI_ASSERT(Admin == wsl::windows::common::security::IsTokenElevated(UserToken));

    // Ensure that the path has a trailing path separator.
    std::wstring sharePath{Path};
    if (sharePath.back() != L'\\')
    {
        sharePath += L'\\';
    }

    // Check if a matching share already exists.
    bool created = false;
    std::wstring tag;
    VirtioFsShare key(sharePath.c_str(), Options, Admin);
    if (!m_virtioFsShares.contains(key))
    {
        // Generate a new tag for the share.
        tag = Admin ? TEXT(LX_INIT_DRVFS_ADMIN_VIRTIO_TAG) : TEXT(LX_INIT_DRVFS_VIRTIO_TAG);
        tag += sharePath[0];
        tag += std::to_wstring(m_virtioFsShares.size());
        WI_ASSERT(!FindVirtioFsShare(tag.c_str(), Admin));

        (void)m_guestDeviceManager->AddGuestDevice(
            VIRTIO_FS_DEVICE_ID,
            Admin ? VIRTIO_FS_ADMIN_CLASS_ID : VIRTIO_FS_CLASS_ID,
            tag.c_str(),
            key.OptionsString().c_str(),
            sharePath.c_str(),
            VIRTIO_FS_FLAGS_TYPE_FILES,
            UserToken);

        m_virtioFsShares.emplace(std::move(key), tag);
        created = true;
    }
    else
    {
        tag = m_virtioFsShares[key];
    }

    WSL_LOG(
        "WslCoreVmAddVirtioFsShare",
        TraceLoggingValue(Admin, "admin"),
        TraceLoggingValue(sharePath.c_str(), "path"),
        TraceLoggingValue(Options, "options"),
        TraceLoggingValue(tag.c_str(), "tag"),
        TraceLoggingValue(created, "created"),
        TraceLoggingValue(m_virtioFsShares.size(), "shareCount"));

    return tag;
}

void WslCoreVm::OnCrash(_In_ LPCWSTR Details)
{
    if (m_vmCrashEvent.is_signaled())
    {
        return; // Crash information has already been collected
    }

    WSL_LOG("GuestCrash", TraceLoggingValue(Details, "Data"));
    const auto crashInformation = wsl::shared::FromJson<wsl::windows::common::hcs::CrashReport>(Details);

    if (m_vmConfig.MaxCrashDumpCount >= 0)
    {
        constexpr auto c_extension = L".txt";
        constexpr auto c_prefix = L"kernel-panic-";
        const auto filename = std::format(L"{}{}-{}{}", c_prefix, std::time(nullptr), m_runtimeId, c_extension);
        auto tracePath = m_vmConfig.CrashDumpFolder / filename;

        auto runAsUser = wil::impersonate_token(m_userToken.get());

        std::error_code error;
        std::filesystem::create_directories(m_vmConfig.CrashDumpFolder, error);
        if (error.value())
        {
            THROW_WIN32_MSG(error.value(), "Failed to create folder: %ls", m_vmConfig.CrashDumpFolder.c_str());
        }

        auto pred = [&c_extension, &c_prefix](const auto& e) {
            return WI_IsFlagSet(GetFileAttributes(e.path().c_str()), FILE_ATTRIBUTE_TEMPORARY) && e.path().has_extension() &&
                   e.path().extension() == c_extension && e.path().has_filename() && e.path().filename().wstring().find(c_prefix) == 0;
        };

        wsl::windows::common::wslutil::EnforceFileLimit(m_vmConfig.CrashDumpFolder.c_str(), m_vmConfig.MaxCrashDumpCount, pred);

        {
            std::wofstream outputFile(tracePath.wstring());
            THROW_HR_IF(E_UNEXPECTED, !outputFile || !(outputFile << crashInformation.CrashLog));
        }

        m_vmCrashLogFile = std::move(tracePath);
    }

    m_vmCrashEvent.SetEvent();
}

void WslCoreVm::OnExit(_In_opt_ PCWSTR ExitDetails)
{
    // Indicate that the VM has exited, and wake any waiting threads. The instance may be in its destructor at this
    // point but closing m_system will wait for any outstanding callbacks, so this function will complete before the
    // destructor continues.
    std::function<void(GUID)> terminationCallback{};
    {
        auto exitLock = m_exitCallbackLock.lock_exclusive();
        if (ARGUMENT_PRESENT(ExitDetails))
        {
            m_exitDetails = ExitDetails;
        }

        m_vmExitEvent.SetEvent();

        // If we reach this block and 'm_terminatingEvent' is not signaled, then this is abnormal shutdown.
        // If that happens, set m_terminatingEvent so all pending socket operations can be properly cancelled.
        if (!m_terminatingEvent.is_signaled())
        {
            WSL_LOG("AbnormalVmExit", TraceLoggingValue(ExitDetails, "Details"));
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
    const auto& info = m_miniInitChannel.ReceiveMessage<LX_INIT_GUEST_CAPABILITIES>();

    m_kernelVersionString = wsl::shared::string::MultiByteToWide(info.Buffer);

    // Parse the version string.
    const std::regex pattern("(\\d+)\\.(\\d+)\\.(\\d+).*");
    std::smatch match;
    const std::string input = info.Buffer;
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
        THROW_HR_MSG(E_UNEXPECTED, "Failed to parse kernel version: '%hs', %hs", info.Buffer, e.what());
    }

    m_seccompAvailable = info.SeccompAvailable;
    WSL_LOG(
        "GuestKernelInfo",
        TraceLoggingValue(m_seccompAvailable, "SeccompAvailable"),
        TraceLoggingValue(std::get<0>(m_kernelVersion), "Version"),
        TraceLoggingValue(std::get<1>(m_kernelVersion), "Revision"),
        TraceLoggingValue(std::get<2>(m_kernelVersion), "Minor"));
}

ULONG WslCoreVm::ReserveLun(_In_ std::optional<ULONG> Lun)
{
    if (Lun.has_value() && !m_lunBitmap[Lun.value()])
    {
        m_lunBitmap[Lun.value()] = true;
        return Lun.value();
    }

    for (ULONG index = 0; index < gsl::narrow_cast<ULONG>(m_lunBitmap.size()); index += 1)
    {
        if (!m_lunBitmap[index])
        {
            m_lunBitmap[index] = true;
            return index;
        }
    }

    THROW_HR(WSL_E_TOO_MANY_DISKS_ATTACHED);
}

void WslCoreVm::RestorePassthroughDiskState(_In_ LPCWSTR Disk) const
try
{
    const auto diskHandle = wsl::windows::common::disk::OpenDevice(Disk, GENERIC_READ | GENERIC_WRITE, m_vmConfig.MountDeviceTimeout);
    wsl::windows::common::disk::SetOnline(diskHandle.get(), true, m_vmConfig.MountDeviceTimeout);
    return;
}
CATCH_LOG()

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
        auto listenSocket = wsl::windows::common::hvsocket::Listen(m_runtimeId, LX_INIT_UTILITY_VM_VIRTIOFS_PORT);
        m_virtioFsThread = std::thread(&WslCoreVm::VirtioFsWorker, this, std::move(listenSocket));
    }
}

void WslCoreVm::ResizeDistribution(_In_ ULONG Lun, _In_ HANDLE OutputHandle, _In_ ULONG64 NewSize)
{
    auto lock = m_lock.lock_exclusive();

    LX_MINI_INIT_RESIZE_DISTRIBUTION_MESSAGE message;
    message.Header.MessageSize = sizeof(message);
    message.Header.MessageType = LxMiniInitMessageResizeDistribution;
    message.ScsiLun = Lun;
    message.NewSize = NewSize;

    m_miniInitChannel.SendMessage(message);

    wsl::shared::SocketChannel channel{AcceptConnection(m_vmConfig.KernelBootTimeout), "ResizeDistribution", m_terminatingEvent.get()};
    auto outputChannel = AcceptConnection(m_vmConfig.KernelBootTimeout);

    wsl::windows::common::relay::ScopedRelay outputRelay(std::move(outputChannel), OutputHandle);

    const auto& resultMessage = channel.ReceiveMessage<LX_MINI_INIT_RESIZE_DISTRIBUTION_RESPONSE>();
    if (resultMessage.ResponseCode != 0)
    {
        THROW_HR_WITH_USER_ERROR(E_FAIL, wsl::shared::Localization::MessageFailedToResizeDisk());
    }
}

void WslCoreVm::SaveAttachedDisksState()
try
{
    auto lock = m_lock.lock_exclusive();
    const auto key = wsl::windows::common::registry::OpenOrCreateLxssDiskMountsKey(&m_userSid.Sid);
    for (const auto& e : m_attachedDisks)
    {
        if (e.first.User)
        {
            SaveDiskState(key.get(), e.first, e.second, e.first.Type);
        }
    }

    return;
}
CATCH_LOG()

void WslCoreVm::SaveDiskState(_In_ HKEY Key, _In_ const AttachedDisk& Disk, _In_ const DiskState& State, _In_ const DiskType& SaveDiskType)
{
    const auto keyPath = std::to_wstring(State.Lun);
    const auto diskKey = wsl::windows::common::registry::CreateKey(Key, keyPath.c_str(), KEY_ALL_ACCESS, nullptr, REG_OPTION_VOLATILE);

    wsl::windows::common::registry::WriteString(diskKey.get(), nullptr, c_diskValueName, Disk.Path.c_str());

    wsl::windows::common::registry::WriteDword(diskKey.get(), nullptr, c_disktypeValueName, static_cast<DWORD>(SaveDiskType));

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

std::pair<int, LX_MINI_MOUNT_STEP> WslCoreVm::UnmountDisk(_In_ const AttachedDisk& Disk, _Inout_ DiskState& State)
{
    // Iterate through the mountpoints to unmount and delete them
    for (auto it = State.Mounts.begin(); it != State.Mounts.end(); it = State.Mounts.erase(it))
    {
        const auto result = UnmountVolume(Disk, it->first, it->second.Name.c_str());
        if (result.first != 0)
        {
            return result;
        }
    }

    // Tell the guest to flush its IO caches and stop using the disk.
    LX_MINI_INIT_DETACH_MESSAGE message;
    message.Header.MessageType = LxMiniInitMessageDetach;
    message.Header.MessageSize = sizeof(message);
    message.ScsiLun = State.Lun;

    m_miniInitChannel.SendMessage(message);

    // Accept a connection from mini_init.
    wsl::shared::SocketChannel channel{AcceptConnection(m_vmConfig.KernelBootTimeout), "MountResult", m_terminatingEvent.get()};

    // Get the unmount result from mini_init
    return GetMountResult(channel);
}

std::pair<int, LX_MINI_MOUNT_STEP> WslCoreVm::UnmountVolume(_In_ const AttachedDisk& Disk, _In_ ULONG PartitionIndex, _In_ PCWSTR Name)
{
    wsl::shared::MessageWriter<LX_MINI_INIT_UNMOUNT_MESSAGE> message(LxMiniInitMessageUnmount);
    message.WriteString(Name);

    // Send the message.
    m_miniInitChannel.SendMessage<LX_MINI_INIT_UNMOUNT_MESSAGE>(message.Span());

    // Accept a connection from mini_init.
    wsl::shared::SocketChannel channel{AcceptConnection(m_vmConfig.KernelBootTimeout), "MountResult", m_terminatingEvent.get()};

    // Get the unmount result from mini_init.
    return GetMountResult(channel);
}

_Requires_lock_held_(m_guestDeviceLock)
void WslCoreVm::VerifyPlan9Servers()
{
    for (auto it = m_plan9Servers.begin(); it != m_plan9Servers.end();)
    {
        const HRESULT result = it->second->IsRunning();

        // If the server process was terminated (which can happen e.g. if the user logged out and
        // back in), attempting to make a COM call will return
        // HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE). For this and other errors, remove the
        // server from the list and mark DrvFs for that port uninitialized.
        // N.B. The call will return S_FALSE if the server is not running. That should never
        //      happen since this service never calls Pause(), but in case it does that is also
        //      treated as an error.
        if (result != S_OK)
        {
            if (it->first == LX_INIT_UTILITY_VM_PLAN9_DRVFS_ADMIN_PORT)
            {
                m_adminDrvfsToken.reset();
            }
            else
            {
                WI_ASSERT(it->first == LX_INIT_UTILITY_VM_PLAN9_DRVFS_PORT);

                m_drvfsToken.reset();
            }

            it = m_plan9Servers.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void WslCoreVm::VirtioFsWorker(_In_ const wil::unique_socket& listenSocket)
try
{
    wsl::windows::common::wslutil::SetThreadDescription(L"VirtioFs - Worker");

    for (;;)
    {
        // Create a worker thread to handle each request.
        wsl::shared::SocketChannel channel{
            wsl::windows::common::hvsocket::Accept(listenSocket.get(), INFINITE, m_terminatingEvent.get()),
            "VirtioFs",
            m_terminatingEvent.get()};
        std::thread([this, channel = std::move(channel)]() mutable {
            try
            {
                wsl::windows::common::wslutil::SetThreadDescription(L"VirtioFs - Request");

                auto [message, span] = channel.ReceiveMessageOrClosed<MESSAGE_HEADER>();
                if (message == nullptr)
                {
                    return;
                }

                auto respondWithTag = [&](const std::wstring& tag, HRESULT result) {
                    // Respond to the guest with the tag that should be used to mount the device.

                    wsl::shared::MessageWriter<LX_INIT_ADD_VIRTIOFS_SHARE_RESPONSE_MESSAGE> response(LxInitMessageAddVirtioFsDeviceResponse);
                    response->Result = SUCCEEDED(result) ? 0 : EINVAL; // TODO: Improved HRESULT -> errno mapping.
                    response.WriteString(response->TagOffset, tag);

                    channel.SendMessage<LX_INIT_ADD_VIRTIOFS_SHARE_RESPONSE_MESSAGE>(response.Span());
                };

                if (message->MessageType == LxInitMessageAddVirtioFsDevice)
                {
                    std::wstring tag;
                    const auto result = wil::ResultFromException([this, span, &tag]() {
                        const auto* addShare = gslhelpers::try_get_struct<LX_INIT_ADD_VIRTIOFS_SHARE_MESSAGE>(span);
                        THROW_HR_IF(E_UNEXPECTED, !addShare);

                        const auto path = wsl::shared::string::FromSpan(span, addShare->PathOffset);
                        THROW_HR_IF_MSG(E_INVALIDARG, !wsl::shared::string::IsDriveRoot(path), "%hs is not the root of a drive", path);

                        const auto pathWide = wsl::shared::string::MultiByteToWide(path);
                        const auto options = wsl::shared::string::FromSpan(span, addShare->OptionsOffset);
                        const auto optionsWide = wsl::shared::string::MultiByteToWide(options);

                        // Acquire the lock and attempt to add the device.
                        auto guestDeviceLock = m_guestDeviceLock.lock_exclusive();
                        tag = AddVirtioFsShare(addShare->Admin, pathWide.c_str(), optionsWide.c_str());
                    });

                    respondWithTag(tag, result);
                }
                else if (message->MessageType == LxInitMessageRemountVirtioFsDevice)
                {
                    std::wstring newTag;
                    const auto result = wil::ResultFromException([this, span, &newTag]() {
                        const auto* remountShare = gslhelpers::try_get_struct<LX_INIT_REMOUNT_VIRTIOFS_SHARE_MESSAGE>(span);
                        THROW_HR_IF(E_UNEXPECTED, !remountShare);

                        const std::string tag = wsl::shared::string::FromSpan(span, remountShare->TagOffset);
                        if (tag.find(LX_INIT_DRVFS_ADMIN_VIRTIO_TAG, 0) == 0)
                        {
                            THROW_HR_IF(E_UNEXPECTED, remountShare->Admin);
                        }
                        else if (tag.find(LX_INIT_DRVFS_VIRTIO_TAG, 0) == 0)
                        {
                            THROW_HR_IF(E_UNEXPECTED, !remountShare->Admin);
                        }
                        else
                        {
                            THROW_HR_MSG(E_UNEXPECTED, "Unexpected tag %hs", tag.data());
                        }

                        const auto tagWide = wsl::shared::string::MultiByteToWide(tag);
                        auto guestDeviceLock = m_guestDeviceLock.lock_exclusive();
                        const auto foundShare = FindVirtioFsShare(tagWide.c_str(), !remountShare->Admin);
                        THROW_HR_IF_MSG(E_UNEXPECTED, !foundShare.has_value(), "Unknown tag %ls", tagWide.c_str());

                        newTag = AddVirtioFsShare(remountShare->Admin, foundShare->Path.c_str(), foundShare->OptionsString().c_str());
                    });

                    respondWithTag(newTag, result);
                }
                else
                {
                    THROW_HR_MSG(E_UNEXPECTED, "Unexpected MessageType %d", message->MessageType);
                }
            }
            CATCH_LOG()
        }).detach();
    }
}
CATCH_LOG()

std::string WslCoreVm::s_GetMountTargetName(_In_ PCWSTR Disk, _In_opt_ PCWSTR Name, _In_ int PartitionIndex)
{
    // Derive the mount target from the disk and partition names.
    // The format is <Disk>p[partition]
    // For Example: PhysicalDisk1p2
    // If user has specified the name, ensure proper formatting and use it instead
    if (ARGUMENT_PRESENT(Name))
    {
        auto mountName = wsl::shared::string::WideToMultiByte(Name);
        // Throw if the name contains '/' since it is a linux path separator
        THROW_HR_IF(WSL_E_VM_MODE_INVALID_MOUNT_NAME, mountName.find('/') != std::string::npos);
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

LX_INIT_DRVFS_MOUNT WslCoreVm::s_InitializeDrvFs(_Inout_ WslCoreVm* VmContext, _In_ HANDLE UserToken)
{
    try
    {
        return VmContext->InitializeDrvFs(UserToken) ? LxInitDrvfsMountElevated : LxInitDrvfsMountNonElevated;
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();

        return LxInitDrvfsMountNone;
    }
}

void CALLBACK WslCoreVm::s_OnExit(_In_ HCS_EVENT* Event, _In_opt_ void* Context)
try
{
    const auto utilityVm = static_cast<WslCoreVm*>(Context);
    if (Event->Type == HcsEventSystemCrashInitiated || Event->Type == HcsEventSystemCrashReport)
    {
        utilityVm->OnCrash(Event->EventData);
    }
    else if ((Event->Type == HcsEventSystemExited) || (Event->Type == HcsEventServiceDisconnect))
    {
        utilityVm->OnExit(Event->EventData);
    }
}
CATCH_LOG();

bool WslCoreVm::AttachedDisk::operator<(const AttachedDisk& other) const
{
    if (Type < other.Type)
    {
        return true;
    }

    if (Type == other.Type)
    {
        return _wcsicmp(Path.c_str(), other.Path.c_str()) < 0;
    }

    return false;
}

bool WslCoreVm::AttachedDisk::operator==(const AttachedDisk& other) const
{
    return Type == other.Type && wsl::windows::common::string::IsPathComponentEqual(Path, other.Path);
}

WslCoreVm::VirtioFsShare::VirtioFsShare(PCWSTR Path, PCWSTR Options, bool Admin) : Path(Path), Admin(Admin)
{
    // Parse the options string into a map representing mount options to ensure that shares with functionally
    // identical options can share a single device.
    // For example: "uid=1000;gid=1000" and "gid=1000;uid=1000"
    auto optionsVector = wsl::shared::string::Split(std::wstring{Options}, L';');
    for (const auto& option : optionsVector)
    {
        std::wstring key;
        std::wstring value;
        const auto pos = option.find_first_of(L'=');
        if (pos == option.npos)
        {
            key = option;
        }
        else
        {
            key = option.substr(0, pos);
            value = option.substr(pos + 1);
        }

        if (!key.empty())
        {
            this->Options.insert({std::move(key), std::move(value)});
        }
    }

    if constexpr (wsl::shared::Debug)
    {
        const auto originalSet = std::set<std::wstring>(optionsVector.begin(), optionsVector.end());
        auto newVector = wsl::shared::string::Split(OptionsString(), L';');
        const auto newSet = std::set<std::wstring>(newVector.begin(), newVector.end());
        WI_ASSERT_MSG(originalSet == newSet, "mount options do not match");
    }
}

std::wstring WslCoreVm::VirtioFsShare::OptionsString() const
{
    std::wstring optionsString;
    for (const auto& option : Options)
    {
        if (!optionsString.empty())
        {
            optionsString += L';';
        }

        optionsString += option.first;
        if (!option.second.empty())
        {
            optionsString += L'=';
            optionsString += option.second;
        }
    }

    return optionsString;
}

bool WslCoreVm::VirtioFsShare::operator<(const VirtioFsShare& other) const
{
    return std::tie(Path, Options, Admin) < std::tie(other.Path, other.Options, other.Admin);
}

bool WslCoreVm::VirtioFsShare::operator==(const VirtioFsShare& other) const
{
    return Path == other.Path && Options == other.Options && Admin == other.Admin;
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

    // If DNS tunneling was requested, ensure it is supported by Windows.
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
