// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    OpenVmmVirtualMachine.cpp

Abstract:

    Implementation of IWSLCVirtualMachine using OpenVMM as the VMM backend.

    Spawns openvmm.exe in ttrpc orchestration mode and configures the VM via
    vmservice RPCs (CreateVM, ResumeVM, ModifyResource, etc.).

    Current limitations:
    - AddShare/RemoveShare require vmservice.proto extensions.
    - GPU passthrough is not supported.

--*/

#include "precomp.h"

#include "OpenVmmVirtualMachine.h"
#include <format>
#include <filesystem>
#include <afunix.h>
#include "wslutil.h"
#include "lxinitshared.h"
#include "ConsommeNetworking.h"

using namespace wsl::windows::common;
using wsl::windows::service::wslc::OpenVmmVirtualMachine;
namespace wslutil = wsl::windows::common::wslutil;

OpenVmmVirtualMachine::OpenVmmVirtualMachine(_In_ const WSLCSessionSettings* Settings)
{
    THROW_HR_IF(E_POINTER, Settings == nullptr);

    std::lock_guard lock(m_lock);

    THROW_IF_FAILED(CoCreateGuid(&m_vmId));
    m_vmIdString = wsl::shared::string::GuidToString<wchar_t>(m_vmId, wsl::shared::string::GuidToStringFlags::Uppercase);
    m_featureFlags = Settings->FeatureFlags;

    // Disable features not yet supported by the OpenVMM backend.
    WI_ClearFlag(m_featureFlags, WslcFeatureFlagsGPU);

    m_networkingMode = Settings->NetworkingMode;
    m_bootTimeoutMs = Settings->BootTimeoutMs;
    m_cpuCount = Settings->CpuCount;
    m_memoryMb = Settings->MemoryMb;

    // Configure termination callback
    if (Settings->TerminationCallback)
    {
        m_terminationCallback = Settings->TerminationCallback;
    }

    // Resolve paths for kernel, initrd, and root VHD.
    auto basePath = wslutil::GetBasePath();

#ifdef WSL_KERNEL_PATH
    m_kernelPath = std::filesystem::path(WSL_KERNEL_PATH);
#else
    m_kernelPath = basePath / L"tools" / L"vmlinux";
    if (!std::filesystem::exists(m_kernelPath))
    {
        // Fall back to the standard kernel name if vmlinux is not found.
        m_kernelPath = basePath / L"tools" / LXSS_VM_MODE_KERNEL_NAME;
    }
#endif

    m_initrdPath = basePath / L"tools" / LXSS_VM_MODE_INITRD_NAME;

#ifdef WSL_KERNEL_MODULES_PATH
    m_modulesVhdPath = std::filesystem::path(TEXT(WSL_KERNEL_MODULES_PATH));
#else
    m_modulesVhdPath = basePath / L"tools" / L"modules.vhd";
#endif

    if (Settings->RootVhdOverride != nullptr)
    {
        m_rootVhdPath = Settings->RootVhdOverride;
    }
    else
    {
#ifdef WSL_SYSTEM_DISTRO_PATH
        m_rootVhdPath = TEXT(WSL_SYSTEM_DISTRO_PATH);
#else
        m_rootVhdPath = std::filesystem::path(wslutil::GetMsiPackagePath().value()) / L"system.vhd";
#endif
    }

    // Locate openvmm.exe. Expect it alongside the WSL binaries.
    m_openvmmPath = basePath / L"openvmm.exe";
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
        !std::filesystem::exists(m_openvmmPath),
        "openvmm.exe not found at: %ls",
        m_openvmmPath.c_str());

    // Pre-create the container storage VHDX so it's ready for hot-attach.
    // WSLCSession::ConfigureStorage will attach, format, and mount it later.
    if (Settings->StoragePath != nullptr)
    {
        std::filesystem::path storagePath{Settings->StoragePath};
        m_storageVhdPath = storagePath / L"storage.vhdx";

        std::filesystem::create_directories(storagePath);
        if (!std::filesystem::exists(m_storageVhdPath))
        {
            VIRTUAL_STORAGE_TYPE storageType{VIRTUAL_STORAGE_TYPE_DEVICE_VHDX, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
            CREATE_VIRTUAL_DISK_PARAMETERS createParams{};
            createParams.Version = CREATE_VIRTUAL_DISK_VERSION_2;
            createParams.Version2.MaximumSize = Settings->MaximumStorageSizeMb * 1024ULL * 1024ULL;
            wil::unique_hfile diskHandle;
            THROW_IF_WIN32_ERROR_MSG(CreateVirtualDisk(
                &storageType, m_storageVhdPath.c_str(), VIRTUAL_DISK_ACCESS_NONE,
                nullptr, CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &createParams, nullptr, &diskHandle),
                "Failed to create storage VHDX: %ls", m_storageVhdPath.c_str());
        }
    }

    // Build kernel command line matching HcsVirtualMachine's format.
    m_kernelCmdLine = L"initrd=\\" LXSS_VM_MODE_INITRD_NAME L" " TEXT(WSLC_ROOT_INIT_ENV) L"=1 panic=-1";
    m_kernelCmdLine += std::format(L" nr_cpus={}", Settings->CpuCount);

    // Append common WSL kernel parameters (timesync, printk, page reporting).
    helpers::AppendCommonKernelCommandLine(m_kernelCmdLine, c_pageReportingOrder);

    // Setup dmesg collector with optional DmesgOutput handle, matching HcsVirtualMachine.
    // The DmesgCollector creates named pipes that we pass to OpenVMM via serial and
    // virtio console configs to capture kernel output.
    wil::unique_handle dmesgOutputHandle;
    if (Settings->DmesgOutput.Handle.File != nullptr && Settings->DmesgOutput.Handle.File != INVALID_HANDLE_VALUE)
    {
        dmesgOutputHandle.reset(wslutil::DuplicateHandle(wslutil::FromCOMInputHandle(Settings->DmesgOutput), GENERIC_WRITE | SYNCHRONIZE));
    }

    m_dmesgCollector = DmesgCollector::Create(
        m_vmId, m_vmExitEvent, true, false, L"", FeatureEnabled(WslcFeatureFlagsEarlyBootDmesg), std::move(dmesgOutputHandle));

    if (FeatureEnabled(WslcFeatureFlagsEarlyBootDmesg))
    {
        // Earlycon captures kernel output via COM1 before the hvc0 driver loads.
        m_kernelCmdLine += L" earlycon=uart8250,io,0x3f8,115200";
    }

    m_kernelCmdLine += L" console=hvc0 debug";

    // Set up vsock bridge path for HvSocket emulation.
    // OpenVMM uses a Unix domain socket for the hybrid_vsock bridge.
    // The hybrid_vsock bridge appends "_<port>" (e.g. "_50000") to this path,
    // and Unix domain sockets have a 108-byte path limit on Windows.
    // The SYSTEM profile temp path is too long, so use a short fixed directory.
    auto vsockDir = std::filesystem::path(c_vsockBridgeDir);
    std::filesystem::create_directories(vsockDir);
    // Use first 8 chars of the GUID to keep it short but unique.
    m_vsockPath = vsockDir / std::format(L"vm-{:.8}", m_vmIdString);

    // Set up the ttrpc socket path for runtime VM management.
    m_ttrpcSocketPath = vsockDir / std::format(L"vm-{:.8}.ttrpc", m_vmIdString);
    DeleteFileW(m_ttrpcSocketPath.c_str());

    // Setup boot VHDs — use the same pattern as HcsVirtualMachine.
    auto attachBootDisk = [&](PCWSTR path) {
        const ULONG lun = AllocateLun();
        DiskInfo disk{path, true};
        m_attachedDisks.emplace(lun, std::move(disk));
    };

    attachBootDisk(m_rootVhdPath.c_str());
    attachBootDisk(m_modulesVhdPath.c_str());

    auto cleanupOnFailure = wil::scope_exit([this]() {
        m_vmExitEvent.SetEvent();

        if (m_vmService)
        {
            m_vmService->Disconnect();
            m_vmService.reset();
        }

        if (m_processHandle)
        {
            TerminateProcess(m_processHandle.get(), 1);
        }

        if (m_processWatchThread.joinable())
        {
            m_processWatchThread.join();
        }

        if (m_initListenSocket != INVALID_SOCKET)
        {
            closesocket(m_initListenSocket);
            m_initListenSocket = INVALID_SOCKET;
        }
        DeleteFileW(m_initListenPath.c_str());

        if (m_crashDumpListenSocket != INVALID_SOCKET)
        {
            closesocket(m_crashDumpListenSocket);
            m_crashDumpListenSocket = INVALID_SOCKET;
        }
        DeleteFileW(m_crashDumpListenPath.c_str());

        try
        {
            if (!m_ttrpcSocketPath.empty())
            {
                std::filesystem::remove(m_ttrpcSocketPath);
            }
        }
        CATCH_LOG()
    });

    // Create Unix domain socket listeners for the hybrid_vsock bridge BEFORE
    // launching openvmm. The guest connects to vsock ports immediately on boot,
    // and OpenVMM's hybrid_vsock bridge relays connections to the host.
    //
    // The bridge uses the HvSocket GUID template to construct the path:
    // port 50000 (0xC350) becomes GUID 0000c350-facb-11e6-bd58-64006a7986d3,
    // and the bridge looks for <vsock_path>_<guid> on the host.
    std::tie(m_initListenSocket, m_initListenPath) =
        CreateVsockListener(LX_INIT_UTILITY_VM_INIT_PORT);

    std::tie(m_crashDumpListenSocket, m_crashDumpListenPath) =
        CreateVsockListener(LX_INIT_UTILITY_VM_CRASH_DUMP_PORT);

    // Launch the openvmm process.
    LaunchOpenVmm();

    cleanupOnFailure.release();
}

std::pair<SOCKET, std::wstring> OpenVmmVirtualMachine::CreateVsockListener(ULONG port)
{
    auto portHex = std::format(L"{:08x}", port);
    auto listenPath = std::format(L"{}_{}-facb-11e6-bd58-64006a7986d3", m_vsockPath.wstring(), portHex);
    DeleteFileW(listenPath.c_str());

    SOCKET listenSocket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    THROW_LAST_ERROR_IF(listenSocket == INVALID_SOCKET);
    auto closeOnFailure = wil::scope_exit([&] { closesocket(listenSocket); });

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    auto narrowPath = wsl::shared::string::WideToMultiByte(listenPath);
    THROW_HR_IF_MSG(E_INVALIDARG, narrowPath.size() >= sizeof(addr.sun_path),
        "vsock bridge path too long: %hs", narrowPath.c_str());
    memcpy(addr.sun_path, narrowPath.c_str(), narrowPath.size() + 1);

    THROW_LAST_ERROR_IF(bind(listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR);
    THROW_LAST_ERROR_IF(listen(listenSocket, 1) == SOCKET_ERROR);

    WSL_LOG("OpenVmmVsockListenerReady",
        TraceLoggingValue(listenPath.c_str(), "ListenPath"),
        TraceLoggingValue(port, "Port"));

    closeOnFailure.release();
    return {listenSocket, std::move(listenPath)};
}

std::wstring OpenVmmVirtualMachine::BuildCommandLine() const
{
    std::wstring cmd = std::format(L"\"{}\"", m_openvmmPath.wstring());
    cmd += std::format(L" --ttrpc \"{}\"", m_ttrpcSocketPath.wstring());

    return cmd;
}

void OpenVmmVirtualMachine::ConfigureVmService() const
{
    THROW_IF_FAILED(m_vmService->SetKernelPath(m_kernelPath.c_str()));
    THROW_IF_FAILED(m_vmService->SetInitrdPath(m_initrdPath.c_str()));

    // Kernel command line — the server prepends "panic=-1 debug pci=off console=ttyS0 "
    // automatically via HyperVGen2LinuxDirect chipset type.
    THROW_IF_FAILED(m_vmService->SetKernelCmdLine(m_kernelCmdLine.c_str()));

    // Ensure 2MB granularity. Cap at 4GB because OpenVMM on WHP allocates guest RAM upfront.
    constexpr ULONG c_maxMemoryMb = 4096;
    THROW_IF_FAILED(m_vmService->SetMemoryMb((std::min(m_memoryMb, c_maxMemoryMb) & ~0x1)));

    THROW_IF_FAILED(m_vmService->SetProcessorCount(m_cpuCount));

    // HvSocket bridge via vsock path (for the guest init connection).
    THROW_IF_FAILED(m_vmService->SetHvSocketPath(m_vsockPath.c_str()));

    // Boot disks: root VHD (LUN 0) and modules VHD (LUN 1), both read-only.
    for (const auto& [lun, disk] : m_attachedDisks)
    {
        THROW_IF_FAILED(m_vmService->AddBootDisk(0, lun, disk.Path.c_str(), disk.ReadOnly));
    }

    if (m_networkingMode == WSLCNetworkingModeConsomme)
    {
        // Generate a deterministic NIC instance ID from the VM ID so it's
        // stable across restarts but unique per VM.
        GUID nicGuid = m_vmId;
        nicGuid.Data1 ^= c_nicGuidXorMask;

        auto nicIdStr = wsl::shared::string::GuidToString<wchar_t>(nicGuid, wsl::shared::string::GuidToStringFlags::None);
        auto macStr = wsl::shared::string::MultiByteToWide(c_defaultConsommeMacAddress);
        THROW_IF_FAILED(m_vmService->SetConsommeNic(nicIdStr.c_str(), macStr.c_str()));
    }

    // COM1 (port 0) — earlycon output before hvc0 loads. Only configure it when
    // early-boot logging is enabled; otherwise EarlyConsoleName() is empty and the
    // OpenVMM server would fail trying to connect to an empty serial socket path,
    // aborting CreateVM.
    if (const auto earlyConsoleName = m_dmesgCollector->EarlyConsoleName(); !earlyConsoleName.empty())
    {
        THROW_IF_FAILED(m_vmService->AddSerialPort(0, earlyConsoleName.c_str()));
    }

    // Virtio console (/dev/hvc0) — primary console after boot.
    THROW_IF_FAILED(m_vmService->SetVirtioConsolePath(m_dmesgCollector->VirtioConsoleName().c_str()));
}

void OpenVmmVirtualMachine::LaunchOpenVmm()
{
    auto cmd = BuildCommandLine();

    WSL_LOG("LaunchOpenVmm", TraceLoggingValue(cmd.c_str(), "cmd"));

    SubProcess process(m_openvmmPath.c_str(), cmd.c_str());

    // Set OPENVMM_LOG so the openvmm tracing subscriber emits detailed logs.
    // Without this, only INFO-level messages appear (the default), which omits
    // most operational output from VM creation and runtime.
    // The variable is set in the current process environment and inherited by the
    // child; restore it after Start() to avoid polluting the service environment.
    wil::unique_hlocal_string previousLog;
    DWORD prevLen = GetEnvironmentVariableW(L"OPENVMM_LOG", nullptr, 0);
    if (prevLen > 0)
    {
        previousLog.reset(static_cast<PWSTR>(LocalAlloc(LMEM_FIXED, prevLen * sizeof(WCHAR))));
        THROW_IF_NULL_ALLOC(previousLog.get());
        GetEnvironmentVariableW(L"OPENVMM_LOG", previousLog.get(), prevLen);
    }

    SetEnvironmentVariableW(L"OPENVMM_LOG", L"info,openvmm=debug");
    auto restoreEnv = wil::scope_exit([&] {
        SetEnvironmentVariableW(L"OPENVMM_LOG", previousLog.get());
    });

    // Redirect stdout and stderr to a log file for diagnostics.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    auto logPath = m_vsockPath.wstring() + L".log";
    wil::unique_hfile logFile{CreateFileW(
        logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};

    // Duplicate the log file handle for stderr so that stdout and stderr are
    // independent.  OpenVMM closes stdout after startup (pal::close_stdout),
    // and if both handles share the same value that also invalidates stderr,
    // silencing all tracing output.
    wil::unique_hfile logFileForStderr;
    THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(
        GetCurrentProcess(), logFile.get(),
        GetCurrentProcess(), logFileForStderr.put(),
        0, TRUE, DUPLICATE_SAME_ACCESS));

    process.SetStdHandles(nullptr, logFile.get(), logFileForStderr.get());

    // Start the process. The returned handle is the process handle.
    m_processHandle = process.Start();

    // Kill-on-close job object ensures the child is terminated if the service
    // exits without running our destructor.
    m_jobObject.reset(CreateJobObjectW(nullptr, nullptr));
    THROW_LAST_ERROR_IF(!m_jobObject);

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
    jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(
        m_jobObject.get(), JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits)));
    THROW_IF_WIN32_BOOL_FALSE(AssignProcessToJobObject(m_jobObject.get(), m_processHandle.get()));

    logFile.reset();

    // Monitor the openvmm process and signal m_vmExitEvent on exit.
    m_processWatchThread = std::thread(&OpenVmmVirtualMachine::WatchProcessExit, this);

    m_vmService = std::make_unique<WslVmServiceClient>();
    THROW_IF_FAILED_MSG(
        m_vmService->Connect(m_ttrpcSocketPath.c_str(), 30000),
        "Failed to connect to OpenVMM ttrpc server");

    ConfigureVmService();
    THROW_IF_FAILED_MSG(
        m_vmService->CreateVm(),
        "Failed to create VM via ttrpc CreateVM");

    THROW_IF_FAILED_MSG(
        m_vmService->ResumeVm(),
        "Failed to resume VM via ttrpc ResumeVM");

}

void OpenVmmVirtualMachine::WatchProcessExit()
{
    WaitForSingleObject(m_processHandle.get(), INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(m_processHandle.get(), &exitCode);

    WSL_LOG(
        "OpenVmmProcessExited",
        TraceLoggingValue(exitCode, "ExitCode"),
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"));

    m_vmExitEvent.SetEvent();

    if (m_terminationCallback)
    {
        auto reason = (exitCode == 0) ? WSLCVirtualMachineTerminationReasonShutdown : WSLCVirtualMachineTerminationReasonCrashed;
        auto details = std::format(L"openvmm process exited with code {}", exitCode);
        LOG_IF_FAILED(m_terminationCallback->OnTermination(reason, details.c_str()));
    }
}

OpenVmmVirtualMachine::~OpenVmmVirtualMachine()
{
    WSL_LOG("OpenVmmTerminateVmStart", TraceLoggingValue(m_vmIdString.c_str(), "VmId"));

    // Signal termination to any pending operations.
    m_vmExitEvent.SetEvent();

    // TeardownVM releases all VM resources and unblocks WaitVM.
    if (m_vmService)
    {
        LOG_IF_FAILED(m_vmService->TeardownVm());
        m_vmService->Disconnect();
        m_vmService.reset();
    }

    // Wait for graceful exit, then force-terminate.
    if (m_processHandle)
    {
        if (WaitForSingleObject(m_processHandle.get(), c_processTerminationTimeoutMs) == WAIT_TIMEOUT)
        {
            WSL_LOG("OpenVmmForceTerminate", TraceLoggingValue(m_vmIdString.c_str(), "VmId"));
            TerminateProcess(m_processHandle.get(), 1);
        }
    }

    if (m_processWatchThread.joinable())
    {
        m_processWatchThread.join();
    }

    if (m_initListenSocket != INVALID_SOCKET)
    {
        closesocket(m_initListenSocket);
        m_initListenSocket = INVALID_SOCKET;
    }
    DeleteFileW(m_initListenPath.c_str());

    if (m_crashDumpListenSocket != INVALID_SOCKET)
    {
        closesocket(m_crashDumpListenSocket);
        m_crashDumpListenSocket = INVALID_SOCKET;
    }
    DeleteFileW(m_crashDumpListenPath.c_str());

    // Best-effort cleanup of socket files. Use DeleteFileW instead of
    // std::filesystem to avoid exceptions — the files may still be held
    // briefly by the OS after force-terminating the openvmm process.
    DeleteFileW(m_vsockPath.c_str());
    DeleteFileW(m_ttrpcSocketPath.c_str());
}

bool OpenVmmVirtualMachine::FeatureEnabled(WSLCFeatureFlags Value) const
{
    return static_cast<ULONG>(m_featureFlags) & static_cast<ULONG>(Value);
}

HRESULT OpenVmmVirtualMachine::GetId(_Out_ GUID* VmId)
try
{
    *VmId = m_vmId;
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::AcceptConnection(_Out_ HANDLE* Socket)
try
{
    THROW_HR_IF(E_UNEXPECTED, m_initListenSocket == INVALID_SOCKET);

    WSL_LOG("OpenVmmAcceptConnection",
        TraceLoggingValue(m_initListenPath.c_str(), "ListenPath"),
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"));

    wil::unique_event acceptEvent(wil::EventOptions::ManualReset);
    WSAEventSelect(m_initListenSocket, acceptEvent.get(), FD_ACCEPT);

    HANDLE waitHandles[] = { acceptEvent.get(), m_vmExitEvent.get() };
    auto waitResult = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, m_bootTimeoutMs);
    THROW_HR_IF(E_ABORT, waitResult != WAIT_OBJECT_0);

    SOCKET unixSock = accept(m_initListenSocket, nullptr, nullptr);
    THROW_LAST_ERROR_IF(unixSock == INVALID_SOCKET);

    closesocket(m_initListenSocket);
    m_initListenSocket = INVALID_SOCKET;
    DeleteFileW(m_initListenPath.c_str());

    // Return the AF_UNIX socket directly. Callers that wrap it in a
    // SocketChannel should use blocking I/O mode since AF_UNIX on Windows
    // does not support overlapped I/O.
    *Socket = reinterpret_cast<HANDLE>(unixSock);
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::ConfigureNetworking(_In_ HANDLE GnsSocket, _In_opt_ HANDLE* DnsSocket)
try
{
    std::lock_guard lock(m_lock);

    // Consomme networking is configured server-side via NICConfig.
    WI_ASSERT(m_networkingMode == WSLCNetworkingModeConsomme);
    THROW_HR_IF(E_INVALIDARG, m_networkingMode != WSLCNetworkingModeConsomme);
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::AttachDisk(_In_ LPCWSTR Path, _In_ BOOL ReadOnly, _Out_ ULONG* Lun)
try
{
    RETURN_HR_IF(E_POINTER, Path == nullptr || Lun == nullptr);
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !std::filesystem::exists(Path), "Disk path does not exist: '%ls'", Path);

    std::lock_guard lock(m_lock);

    THROW_HR_IF_MSG(E_FAIL, !m_vmService,
        "VM service not available for disk hot-add");

    DiskInfo disk{Path, ReadOnly != FALSE};
    const ULONG allocatedLun = AllocateLun();

    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        FreeLun(allocatedLun);
    });

    THROW_IF_FAILED(m_vmService->AttachScsiDisk(0, allocatedLun, Path, ReadOnly));

    m_attachedDisks.emplace(allocatedLun, std::move(disk));
    cleanup.release();

    *Lun = allocatedLun;
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::DetachDisk(_In_ ULONG Lun)
try
{
    std::lock_guard lock(m_lock);

    auto it = m_attachedDisks.find(Lun);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_attachedDisks.end());

    THROW_HR_IF_MSG(E_FAIL, !m_vmService,
        "VM service not available for disk hot-remove");

    THROW_IF_FAILED(m_vmService->DetachScsiDisk(0, Lun));

    FreeLun(Lun);
    m_attachedDisks.erase(it);

    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::AddShare(_In_ LPCWSTR WindowsPath, _In_ BOOL ReadOnly, _Out_ GUID* ShareId)
try
{
    RETURN_HR_IF(E_POINTER, WindowsPath == nullptr || ShareId == nullptr);
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), !std::filesystem::is_directory(WindowsPath), "Path is not a directory: '%ls'", WindowsPath);

    std::lock_guard lock(m_lock);

    THROW_HR_IF_MSG(E_FAIL, !m_vmService,
        "VM service not available for share add");

    GUID shareIdLocal;
    THROW_IF_FAILED(CoCreateGuid(&shareIdLocal));
    auto shareTag = wsl::shared::string::GuidToString<wchar_t>(shareIdLocal, wsl::shared::string::None);

    WSL_LOG(
        "OpenVmmAddShare",
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"),
        TraceLoggingValue(WindowsPath, "WindowsPath"),
        TraceLoggingValue(ReadOnly, "ReadOnly"),
        TraceLoggingValue(shareTag.c_str(), "Tag"));

    THROW_IF_FAILED(m_vmService->AddShare(shareTag.c_str(), WindowsPath, ReadOnly));

    m_shares.emplace(shareIdLocal, WindowsPath);
    *ShareId = shareIdLocal;
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::RemoveShare(_In_ REFGUID ShareId)
try
{
    std::lock_guard lock(m_lock);

    auto it = m_shares.find(ShareId);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_shares.end());

    THROW_HR_IF_MSG(E_FAIL, !m_vmService,
        "VM service not available for share remove");

    auto shareTag = wsl::shared::string::GuidToString<wchar_t>(it->first, wsl::shared::string::None);

    WSL_LOG(
        "OpenVmmRemoveShare",
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"),
        TraceLoggingValue(shareTag.c_str(), "Tag"));

    THROW_IF_FAILED(m_vmService->RemoveShare(shareTag.c_str()));

    m_shares.erase(it);
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::GetTerminationEvent(_Out_ HANDLE* Event)
try
{
    *Event = wslutil::DuplicateHandle(m_vmExitEvent.get());
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::ConnectToVsockPort(_In_ ULONG Port, _Out_ HANDLE* Socket)
try
{
    WSL_LOG("OpenVmmConnectToVsockPort",
        TraceLoggingValue(Port, "Port"),
        TraceLoggingValue(m_vsockPath.c_str(), "BridgePath"),
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"));

    SOCKET unixSock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    THROW_LAST_ERROR_IF(unixSock == INVALID_SOCKET);
    auto closeUnix = wil::scope_exit([&] { closesocket(unixSock); });

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    auto narrowPath = wsl::shared::string::WideToMultiByte(m_vsockPath.wstring());
    THROW_HR_IF_MSG(E_INVALIDARG, narrowPath.size() >= sizeof(addr.sun_path),
        "vsock bridge path too long: %hs", narrowPath.c_str());
    memcpy(addr.sun_path, narrowPath.c_str(), narrowPath.size() + 1);

    THROW_LAST_ERROR_IF(connect(unixSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR);

    auto connectMsg = std::format("CONNECT {}\n", Port);
    int sent = send(unixSock, connectMsg.c_str(), static_cast<int>(connectMsg.size()), 0);
    THROW_LAST_ERROR_IF(sent == SOCKET_ERROR);
    THROW_HR_IF(E_FAIL, sent != static_cast<int>(connectMsg.size()));

    char response[64]{};
    int totalRead = 0;
    while (totalRead < static_cast<int>(sizeof(response) - 1))
    {
        int n = recv(unixSock, response + totalRead, 1, 0);
        THROW_LAST_ERROR_IF(n == SOCKET_ERROR);
        THROW_HR_IF_MSG(E_FAIL, n == 0, "vsock bridge closed during CONNECT handshake");
        totalRead += n;
        if (response[totalRead - 1] == '\n')
        {
            break;
        }
    }
    response[totalRead] = '\0';

    THROW_HR_IF_MSG(E_FAIL, strncmp(response, "OK ", 3) != 0,
        "vsock bridge CONNECT failed: %hs", response);

    WSL_LOG("OpenVmmConnectToVsockPortOK",
        TraceLoggingValue(Port, "Port"),
        TraceLoggingValue(response, "Response"));

    // Return the AF_UNIX socket directly. Callers that wrap it in a
    // SocketChannel should use blocking I/O mode since AF_UNIX on Windows
    // does not support overlapped I/O.
    closeUnix.release();
    *Socket = reinterpret_cast<HANDLE>(unixSock);
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::AcceptCrashDumpConnection(_Out_ HANDLE* Socket)
try
{
    THROW_HR_IF(E_UNEXPECTED, m_crashDumpListenSocket == INVALID_SOCKET);

    WSL_LOG("OpenVmmAcceptCrashDumpConnection",
        TraceLoggingValue(m_crashDumpListenPath.c_str(), "ListenPath"),
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"));

    wil::unique_event acceptEvent(wil::EventOptions::ManualReset);
    WSAEventSelect(m_crashDumpListenSocket, acceptEvent.get(), FD_ACCEPT);

    HANDLE waitHandles[] = { acceptEvent.get(), m_vmExitEvent.get() };
    auto waitResult = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, INFINITE);
    THROW_HR_IF(E_ABORT, waitResult != WAIT_OBJECT_0);

    SOCKET unixSock = accept(m_crashDumpListenSocket, nullptr, nullptr);
    THROW_LAST_ERROR_IF(unixSock == INVALID_SOCKET);

    // Return the AF_UNIX socket directly. Callers that wrap it in a
    // SocketChannel should use blocking I/O mode since AF_UNIX on Windows
    // does not support overlapped I/O.
    *Socket = reinterpret_cast<HANDLE>(unixSock);
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::MapPort(_In_ int Family, _In_ unsigned short HostPort, _In_ unsigned short GuestPort)
try
{
    std::lock_guard lock(m_lock);

    auto key = std::make_tuple(Family, HostPort, GuestPort);
    if (m_boundPorts.contains(key))
    {
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }

    // Mirror the wslrelay localhost relay limit (see localhost.cpp): the relay's
    // AcceptThread uses WaitForMultipleObjects, which supports at most
    // MAXIMUM_WAIT_OBJECTS (64) handles, with one reserved for the exit event.
    // Reject the mapping if adding it would exceed the limit.
    constexpr size_t c_maxPorts = MAXIMUM_WAIT_OBJECTS - 1;
    if (m_boundPorts.size() >= c_maxPorts)
    {
        return HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES);
    }

    THROW_HR_IF_MSG(E_FAIL, !m_vmService,
        "VM service not available for port bind");

    WSL_LOG(
        "OpenVmmMapPort",
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"),
        TraceLoggingValue(HostPort, "HostPort"),
        TraceLoggingValue(GuestPort, "GuestPort"),
        TraceLoggingValue(Family, "Family"));

    THROW_IF_FAILED(m_vmService->BindPort(HostPort, GuestPort, TRUE, Family));

    m_boundPorts.insert(key);
    return S_OK;
}
CATCH_RETURN()

HRESULT OpenVmmVirtualMachine::UnmapPort(_In_ int Family, _In_ unsigned short HostPort, _In_ unsigned short GuestPort)
try
{
    std::lock_guard lock(m_lock);

    auto key = std::make_tuple(Family, HostPort, GuestPort);
    if (!m_boundPorts.contains(key))
    {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    THROW_HR_IF_MSG(E_FAIL, !m_vmService,
        "VM service not available for port unbind");

    WSL_LOG(
        "OpenVmmUnmapPort",
        TraceLoggingValue(m_vmIdString.c_str(), "VmId"),
        TraceLoggingValue(HostPort, "HostPort"),
        TraceLoggingValue(GuestPort, "GuestPort"),
        TraceLoggingValue(Family, "Family"));

    THROW_IF_FAILED(m_vmService->UnbindPort(HostPort, GuestPort, TRUE, Family));

    m_boundPorts.erase(key);
    return S_OK;
}
CATCH_RETURN()

ULONG OpenVmmVirtualMachine::AllocateLun()
{
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

void OpenVmmVirtualMachine::FreeLun(ULONG Lun)
{
    THROW_HR_IF(E_BOUNDS, Lun >= m_lunBitmap.size());
    THROW_HR_IF(E_INVALIDARG, !m_lunBitmap[Lun]);

    m_lunBitmap[Lun] = false;
}
