/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    OpenVmmVirtualMachineBackend.cpp

Abstract:

    Implementation of IVirtualMachineBackend - represents a single OpenVMM-based VM instance.

--*/

#include "precomp.h"
#include "OpenVmmVirtualMachineBackend.h"
#include <afunix.h>
#include <bitset>
#include <ctime>
#include "GuestDeviceManager.h"
#include "GuestConnector.h"
#include "HandleIO.h"
#include "SubProcess.h"
#include "vsock.hpp"
#include "wslopenvmm.h"

using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::common::vm::c_maximumDisks;
using wsl::windows::common::vm::c_mib;
using wsl::windows::common::vm::c_notSupported;

namespace validation = wsl::windows::common::vm::validation;

namespace {

constexpr UINT32 c_rpcTimeoutMs = 30000;
constexpr std::wstring_view c_savedStatePrefix = L"saved-state-";
constexpr std::wstring_view c_savedStateExtension = L".vmrs";
// Hybrid vsock embeds the AF_VSOCK port in the first field of this AF_HYPERV service ID.
constexpr std::wstring_view c_vsockServiceIdSuffix = L"-facb-11e6-bd58-64006a7986d3";

using validation::ValidateResourceId;

std::unique_ptr<ExecutionContext> CreateExecutionContext(Context Context)
{
    const auto* current = ExecutionContext::Current();
    if (current != nullptr && current->CurrentContext() >= static_cast<ULONGLONG>(Context))
    {
        return {};
    }

    return std::make_unique<ExecutionContext>(Context);
}

void DestroyConfig(WslOpenVmmConfig* Config) noexcept
{
    WslOpenVmmDestroyConfig(&Config);
}

using UniqueConfig = wil::unique_any<WslOpenVmmConfig*, decltype(&DestroyConfig), DestroyConfig>;

void DeleteOwnedFile(const std::filesystem::path& Path) noexcept
{
    if (!Path.empty() && !DeleteFileW(Path.c_str()))
    {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        {
            LOG_WIN32(error);
        }
    }
}

std::filesystem::path PrepareCrashDumpPath(const VmCrashCaptureRequest& Request, const VmInstanceId& Identity)
{
    auto runAsUser = wil::impersonate_token(Identity.UserToken.get());
    wsl::windows::common::filesystem::EnsureDirectory(Request.SavedStateFolder->c_str());
    const auto predicate = [](const auto& entry) {
        return entry.path().has_extension() && entry.path().extension() == c_savedStateExtension && entry.path().has_filename() &&
               entry.path().filename().wstring().starts_with(c_savedStatePrefix) && entry.file_size() > 0;
    };
    wsl::windows::common::wslutil::EnforceFileLimit(Request.SavedStateFolder->c_str(), static_cast<size_t>(Request.MaxSavedStateCount), predicate);

    const auto vmId = wsl::shared::string::GuidToString<wchar_t>(Identity.VmId, wsl::shared::string::GuidToStringFlags::None);
    return Request.SavedStateFolder.value() / std::format(L"{}{}-{}{}", c_savedStatePrefix, std::time(nullptr), vmId, c_savedStateExtension);
}

std::filesystem::path GetVsockListenerPath(const std::filesystem::path& VsockPath, GuestServicePort Port)
{
    return std::format(L"{}_{:08x}{}", VsockPath.native(), Port.Value, c_vsockServiceIdSuffix);
}

} // namespace

OpenVmmVirtualMachineBackend::GuestListener::~GuestListener() noexcept
{
    Socket.reset();
    SocketFile.reset();
}

std::optional<wil::unique_socket> OpenVmmVirtualMachineBackend::GuestListener::Accept()
{
    return wsl::windows::common::socket::CancellableAccept(Socket.get(), INFINITE, CancellationEvent.get());
}

VmDescription wsl::windows::common::vm::openvmm::ValidateCreateRequest(const VmCreateRequest& Request)
{
    THROW_HR_IF(E_INVALIDARG, IsEqualGUID(Request.Identity.VmId, GUID_NULL));
    THROW_HR_IF_MSG(c_notSupported, wsl::shared::Arm64, "OpenVMM direct boot is currently supported only on x64");
    THROW_HR_IF(c_notSupported, Request.Boot.Method == VmBootMethod::Uefi);
    THROW_HR_IF(c_notSupported, Request.Boot.Method != VmBootMethod::Automatic && Request.Boot.Method != VmBootMethod::LinuxDirect);

    VmDescription description;
    description.Identity = Request.Identity;
    description.Backend = BackendKind::OpenVmm;
    description.Processor.Count = Request.Processor.Count;
    // Nested virtualization isn't available: WHP provides no SynIC, which the VMBus devices require.
    THROW_HR_IF_MSG(
        c_notSupported,
        Request.Processor.NestedVirtualization == VmFeatureRequest::Required,
        "OpenVMM does not support nested virtualization");
    description.Processor.NestedVirtualization = false;
    description.Memory.SizeBytes = (Request.Memory.SizeBytes / c_mib) * c_mib;
    validation::ValidateFeature(Request.Processor.PerfmonPmu, L"PMU");
    validation::ValidateFeature(Request.Processor.PerfmonLbr, L"LBR");
    validation::ValidateFeature(Request.Memory.AllowOvercommit, L"memory overcommit");
    validation::ValidateFeature(Request.Memory.DeferredCommit, L"deferred memory commit");
    validation::ValidateFeature(Request.Memory.ColdDiscard, L"cold discard");
    validation::ValidateFeature(Request.Memory.SmallPageBacking, L"small-page memory");
    THROW_HR_IF(
        c_notSupported,
        Request.Memory.FaultClusterSizeShift.has_value() || Request.Memory.DirectMapFaultClusterSizeShift.has_value() ||
            Request.Memory.PageReportingOrder.has_value() || Request.Memory.HostingProcessNameSuffix.has_value());
    THROW_HR_IF(E_INVALIDARG, (Request.Mmio.HighWindowSizeBytes % c_mib) != 0);
    if (Request.Mmio.MaximumGuestAddressBits)
    {
        // OpenVMM allocates the high MMIO gap dynamically, immediately above
        // guest RAM, so the request fits only if RAM plus the gap stays within
        // the address range the guest can reach.
        const auto bits = *Request.Mmio.MaximumGuestAddressBits;
        THROW_HR_IF(E_INVALIDARG, (bits == 0) || (bits >= 64));
        THROW_HR_IF_MSG(
            c_notSupported,
            (description.Memory.SizeBytes + Request.Mmio.HighWindowSizeBytes) > (1ULL << bits),
            "OpenVMM cannot fit the high MMIO gap below the guest address limit");
    }

    if (Request.CrashCapture && Request.CrashCapture->SavedStateFolder)
    {
        THROW_HR_IF(E_INVALIDARG, Request.CrashCapture->SavedStateFolder->empty());
    }

    description.Boot.Method = VmBootMethod::LinuxDirect;
    description.Boot.KernelCommandLine = Request.Boot.KernelCommandLine;

    std::uint32_t nextVirtioConsolePort = 0;
    for (const auto& console : Request.Consoles)
    {
        if (!std::holds_alternative<VmSerialConsole>(console.Device))
        {
            const auto& virtio = std::get<VmVirtioConsole>(console.Device);
            THROW_HR_IF(c_notSupported, virtio.Port != nextVirtioConsolePort || virtio.GuestName != std::format(L"hvc{}", nextVirtioConsolePort));
            ++nextVirtioConsolePort;
        }
        description.Boot.Consoles.push_back(console);
    }

    std::bitset<c_maximumDisks> allocated;
    for (const auto& disk : Request.BootDisks)
    {
        THROW_HR_IF(E_INVALIDARG, disk.Key.empty() || description.BootDisks.contains(disk.Key));
        description.BootDisks.emplace(disk.Key, VmDiskAttachment{});
        validation::ValidateDiskRequest(disk.Disk);
        if (disk.Disk.Placement)
        {
            const auto& placement = *disk.Disk.Placement;
            THROW_HR_IF(c_notSupported, placement.Address.Controller != 0);
            THROW_HR_IF(E_BOUNDS, placement.Address.Lun >= c_maximumDisks);
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), allocated.test(placement.Address.Lun));
            allocated.set(placement.Address.Lun);
        }
    }

    std::uint64_t nextId = 1;
    for (const auto& disk : Request.BootDisks)
    {
        std::uint32_t lun = 0;
        if (disk.Disk.Placement)
        {
            lun = disk.Disk.Placement->Address.Lun;
        }
        else
        {
            while (lun < c_maximumDisks && allocated.test(lun))
            {
                ++lun;
            }
            THROW_HR_IF(WSL_E_TOO_MANY_DISKS_ATTACHED, lun == c_maximumDisks);
            allocated.set(lun);
        }
        description.BootDisks.at(disk.Key) = {
            {description.Identity, nextId++},
            {0, lun},
            VmDiskTransport::Scsi,
            disk.Disk.ReadOnly,
            disk.Disk.UserDisk,
            std::get<VmVirtualDiskSource>(disk.Disk.Source).Path.native(),
            false};
    }

    // OpenVMM's port RPC channel targets only the first Consomme NIC.
    THROW_HR_IF_MSG(c_notSupported, Request.NetworkAdapters.size() > 1, "OpenVMM supports one creation-time network adapter");
    for (const auto& adapter : Request.NetworkAdapters)
    {
        // OpenVMM serves its NIC with a built-in user-mode NAT and has no host network stack to
        // attach an endpoint created on the host to.
        THROW_HR_IF_MSG(
            c_notSupported,
            !std::holds_alternative<VmUserModeNatNetwork>(adapter.Configuration),
            "OpenVMM only supports user-mode NAT networks");

        GUID nicId{};
        THROW_IF_FAILED(CoCreateGuid(&nicId));
        description.NetworkAdapters.emplace(
            adapter.Tag, VmNetworkAttachment{{description.Identity, 1}, adapter.Tag, nicId, adapter.Configuration});
    }

    return description;
}

void OpenVmmVirtualMachineBackend::DestroyVm(WslOpenVmmVm* Vm) noexcept
{
    WslOpenVmmDestroyVm(&Vm);
}

OpenVmmVirtualMachineBackend::OpenVmmVirtualMachineBackend() = default;

OpenVmmVirtualMachineBackend::~OpenVmmVirtualMachineBackend() noexcept
{
    const auto startTimeMs = GetTickCount64();
    WSL_LOG("OpenVmmDestroyVmBegin", TraceLoggingValue(m_description.Identity.VmId, "vmId"));
    {
        auto lock = m_lock.lock_exclusive();
        CloseGuestListenersLocked(m_description.Identity);
    }
    if (m_vm && WaitForSingleObject(m_process.get(), 0) == WAIT_TIMEOUT)
    {
        LOG_IF_FAILED(WslOpenVmmVmTeardown(m_vm.get()));
        LOG_IF_FAILED(WslOpenVmmVmQuit(m_vm.get()));
    }
    m_vm.reset();
    m_job.reset();
    if (m_process)
    {
        // Confirm exit before releasing backing files or deleting socket paths.
        LOG_LAST_ERROR_IF(WaitForSingleObject(m_process.get(), INFINITE) == WAIT_FAILED);
    }
    if (m_processLogThread.joinable())
    {
        m_processLogThread.join();
    }
    if (m_fileSystemResources.DirectoryCreated)
    {
        LOG_IF_FAILED(wil::RemoveDirectoryRecursiveNoThrow(m_fileSystemResources.SocketDirectory.c_str()));
    }
    WSL_LOG(
        "OpenVmmDestroyVmEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(GetTickCount64() - startTimeMs, "durationMs"));
}

std::unique_ptr<OpenVmmVirtualMachineBackend> OpenVmmVirtualMachineBackend::Create(const VmCreateRequest& Request)
{
    const auto context = CreateExecutionContext(Context::CreateVm);
    const auto startTimeMs = GetTickCount64();
    WSL_LOG(
        "OpenVmmCreateVmBegin",
        TraceLoggingValue(Request.Identity.VmId, "vmId"),
        TraceLoggingValue(Request.Processor.Count, "processorCount"),
        TraceLoggingValue(Request.Memory.SizeBytes, "memoryBytes"),
        TraceLoggingValue(Request.BootDisks.size(), "bootDiskCount"),
        TraceLoggingValue(Request.Consoles.size(), "consoleCount"));

    std::unique_ptr<OpenVmmVirtualMachineBackend> backend;
    try
    {
        auto description = wsl::windows::common::vm::openvmm::ValidateCreateRequest(Request);
        backend.reset(new OpenVmmVirtualMachineBackend{});
        backend->m_description = std::move(description);
        backend->Initialize(Request);
        WSL_LOG(
            "OpenVmmCreateVmEnd",
            TraceLoggingValue(Request.Identity.VmId, "vmId"),
            TraceLoggingValue(GetTickCount64() - startTimeMs, "durationMs"));
    }
    catch (...)
    {
        const auto result = wil::ResultFromCaughtException();
        WSL_LOG(
            "OpenVmmCreateVmFailed",
            TraceLoggingValue(Request.Identity.VmId, "vmId"),
            TraceLoggingHResult(result, "result"),
            TraceLoggingValue(GetTickCount64() - startTimeMs, "durationMs"));
        throw;
    }

    return backend;
}

void OpenVmmVirtualMachineBackend::Initialize(const VmCreateRequest& Request)
{
    using namespace wsl::windows::common;
    const auto executable = wslutil::GetBasePath() / L"openvmm.exe";

    auto id = wsl::shared::string::GuidToString<wchar_t>(Request.Identity.VmId, wsl::shared::string::GuidToStringFlags::None);
    std::erase(id, L'-');
    // An exclusive directory creation prevents shortened path IDs from aliasing another VM.
    const auto socketRoot = filesystem::GetTempFolderPath(Request.Identity.UserToken.get());
    {
        const auto runAsUser = wil::impersonate_token(Request.Identity.UserToken.get());
        wil::CreateDirectoryDeep(socketRoot.c_str());
    }
    m_fileSystemResources.SocketDirectory = socketRoot / (L"ov-" + id.substr(0, 16));
    m_fileSystemResources.RpcSocketPath = m_fileSystemResources.SocketDirectory / L"r";
    m_fileSystemResources.VsockPath = m_fileSystemResources.SocketDirectory / L"v";
    constexpr size_t c_guidStringLength = 38;
    constexpr size_t c_guestSocketSuffixLength = 1 + c_guidStringLength;
    const auto vsockPath = wsl::shared::string::WideToMultiByte(m_fileSystemResources.VsockPath.native());
    SOCKADDR_UN address{};
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        vsockPath.size() + c_guestSocketSuffixLength >= sizeof(address.sun_path),
        "OpenVMM guest socket path exceeds the AF_UNIX limit: %hs",
        vsockPath.c_str());

    const auto tokenUser = wil::get_token_information<TOKEN_USER>(Request.Identity.UserToken.get());
    const auto sid = wslutil::SidToString(tokenUser->User.Sid);
    const auto sddl = std::format(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;{})", sid.get());
    wil::unique_hlocal_security_descriptor security;
    THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &security, nullptr));
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.get(), FALSE};
    for (const auto& disk : Request.BootDisks)
    {
        const auto& attachment = m_description.BootDisks.at(disk.Key);
        m_attachedDisks.emplace(attachment.Id.Value, attachment);
    }
    m_nextDiskId = Request.BootDisks.size() + 1;
    {
        const auto runAsUser = wil::impersonate_token(Request.Identity.UserToken.get());
        THROW_IF_WIN32_BOOL_FALSE(CreateDirectoryW(m_fileSystemResources.SocketDirectory.c_str(), &attributes));
    }
    m_fileSystemResources.DirectoryCreated = true;

    UniqueConfig config;
    THROW_IF_FAILED(WslOpenVmmCreateConfig(config.put()));
    THROW_IF_FAILED(WslOpenVmmConfigSetExitOnGuestPowerEvents(config.get()));
    THROW_IF_FAILED(WslOpenVmmConfigSetKernelPath(config.get(), Request.Boot.KernelPath.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetInitrdPath(config.get(), Request.Boot.InitrdPath.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetKernelCmdLine(config.get(), m_description.Boot.KernelCommandLine.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetMemoryMb(config.get(), m_description.Memory.SizeBytes / (1024 * 1024)));
    THROW_IF_FAILED(WslOpenVmmConfigSetHighMmioGapMb(config.get(), Request.Mmio.HighWindowSizeBytes / (1024 * 1024)));
    THROW_IF_FAILED(WslOpenVmmConfigSetProcessorCount(config.get(), Request.Processor.Count));
    THROW_IF_FAILED(WslOpenVmmConfigSetNestedVirt(config.get(), m_description.Processor.NestedVirtualization));
    THROW_IF_FAILED(WslOpenVmmConfigSetHvSocketPath(config.get(), m_fileSystemResources.VsockPath.c_str()));
    if (Request.CrashCapture && Request.CrashCapture->SavedStateFolder)
    {
        const auto crashDumpPath = PrepareCrashDumpPath(Request.CrashCapture.value(), Request.Identity);
        THROW_IF_FAILED(WslOpenVmmConfigSetCrashDumpPath(config.get(), crashDumpPath.c_str()));
    }
    for (const auto& [tag, attachment] : m_description.NetworkAdapters)
    {
        const auto nicId =
            wsl::shared::string::GuidToString<wchar_t>(attachment.GuestInstanceId.value(), wsl::shared::string::GuidToStringFlags::None);
        const auto& configuration = std::get<VmUserModeNatNetwork>(attachment.EffectiveConfiguration);
        const auto macAddress = wsl::shared::string::FormatMacAddress(configuration.ClientMacAddress(), L'-');
        THROW_IF_FAILED(WslOpenVmmConfigSetConsommeNic(config.get(), nicId.c_str(), macAddress.c_str(), L""));
        THROW_IF_FAILED(WslOpenVmmConfigSetConsommeInternalDns(config.get(), nicId.c_str(), configuration.InternalDns));
        m_networkAdapters.emplace(attachment.Id.Value, NetworkAdapter{attachment, {nicId}});
        m_nextDeviceId = attachment.Id.Value + 1;
    }
    for (const auto& disk : Request.BootDisks)
    {
        const auto& attachment = m_description.BootDisks.at(disk.Key);
        THROW_IF_FAILED(WslOpenVmmConfigAddBootDisk(
            config.get(),
            attachment.GuestAddress.Controller,
            attachment.GuestAddress.Lun,
            std::get<VmVirtualDiskSource>(disk.Disk.Source).Path.c_str(),
            disk.Disk.ReadOnly));
    }
    for (const auto& console : Request.Consoles)
    {
        if (const auto* serial = std::get_if<VmSerialConsole>(&console.Device))
        {
            THROW_IF_FAILED(WslOpenVmmConfigAddSerialPort(config.get(), serial->Port, serial->NamedPipe.c_str()));
        }
        else
        {
            const auto& virtio = std::get<VmVirtioConsole>(console.Device);
            // The debug shell pipe has no host-side server, so OpenVMM listens on it (as the HCS VM does).
            // The other consoles are served by the host before the VM is created.
            if (console.Role == VmConsoleRole::DebugShell)
            {
                THROW_IF_FAILED(WslOpenVmmConfigAddVirtioConsoleListener(config.get(), virtio.NamedPipe.c_str()));
            }
            else
            {
                THROW_IF_FAILED(WslOpenVmmConfigAddVirtioConsolePath(config.get(), virtio.NamedPipe.c_str()));
            }
        }
    }

    m_job = helpers::CreateKillOnCloseJob();
    const auto commandLine =
        std::format(L"\"{}\" --rpc \"path={},transport=grpc\"", executable.native(), m_fileSystemResources.RpcSocketPath.native());

    wil::unique_handle primaryToken;
    THROW_IF_WIN32_BOOL_FALSE(DuplicateTokenEx(
        Request.Identity.UserToken.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primaryToken));
    helpers::unique_environment_block environment{nullptr};
    THROW_LAST_ERROR_IF(!CreateEnvironmentBlock(&environment, primaryToken.get(), false));

    SubProcess process{executable.c_str(), commandLine.c_str()};
    process.SetFlags(CREATE_NO_WINDOW);
    process.SetToken(primaryToken.get());
    process.SetEnvironment(environment.get());
    process.SetJobObject(m_job.get());
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    auto input = wsl::windows::common::filesystem::OpenNulDevice(GENERIC_READ);
    helpers::SetHandleInheritable(input.get());
    wil::unique_hfile output{CreateFileW(
        L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    THROW_LAST_ERROR_IF(!output);
    auto [logPipeRead, logPipeWrite] = wslutil::OpenAnonymousPipe(0, true, false);
    THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(logPipeWrite.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
    process.SetStdHandles(input.get(), output.get(), logPipeWrite.get());
    m_process = process.Start();
    logPipeWrite.reset();
    m_processLogThread = std::thread(&OpenVmmVirtualMachineBackend::ReadProcessLog, this, std::move(logPipeRead));
    WSL_LOG(
        "OpenVmmProcessStarted",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(GetProcessId(m_process.get()), "processId"),
        TraceLoggingValue(m_fileSystemResources.SocketDirectory.c_str(), "socketDirectory"));
    const auto startTimeMs = GetTickCount64();
    const auto createResult =
        WslOpenVmmCreateVm(config.addressof(), m_fileSystemResources.RpcSocketPath.c_str(), c_rpcTimeoutMs, m_vm.put());
    WSL_LOG(
        "OpenVmmCreateVmRpc",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingHResult(createResult, "result"),
        TraceLoggingValue(GetTickCount64() - startTimeMs, "durationMs"));
    const auto result = WaitForSingleObject(m_process.get(), 0);
    THROW_LAST_ERROR_IF(result == WAIT_FAILED);
    THROW_HR_WITH_USER_ERROR_IF(WSL_E_VM_CRASHED, wsl::shared::Localization::MessageWSL2Crashed(), result == WAIT_OBJECT_0);
    THROW_IF_FAILED_MSG(createResult, "Failed to create OpenVMM VM");
    {
        auto lock = m_lock.lock_exclusive();
        m_state = VmState::Created;
    }
}

void OpenVmmVirtualMachineBackend::ReadProcessLog(wil::unique_hfile Pipe) noexcept
try
{
    wsl::windows::common::io::MultiHandleWait io;
    io.AddHandle(std::make_unique<wsl::windows::common::io::LineBasedReadHandle>(
        std::move(Pipe),
        [this](const gsl::span<char>& Line) {
            const std::string entry{Line.begin(), Line.end()};
            WSL_LOG(
                "OpenVmmLog", TraceLoggingGuid(m_description.Identity.VmId, "VmId"), TraceLoggingValue(entry.c_str(), "Content"));
        },
        false));
    io.AddHandle(std::make_unique<wsl::windows::common::io::EventHandle>(m_process.get(), [this]() {
        DWORD exitCode = 0;
        LOG_LAST_ERROR_IF(!GetExitCodeProcess(m_process.get(), &exitCode));
        OnProcessExit(exitCode);
    }));
    io.Run(std::nullopt);
}
CATCH_LOG()

void OpenVmmVirtualMachineBackend::OnProcessExit(DWORD ExitCode) noexcept
{
    WSL_LOG(
        "OpenVmmProcessExited", TraceLoggingValue(m_description.Identity.VmId, "vmId"), TraceLoggingValue(ExitCode, "exitCode"));
    {
        auto lock = m_lock.lock_exclusive();
        m_state = VmState::Stopped;
        m_terminationInformation.Reason = ExitCode == ERROR_SUCCESS ? VmTerminationReason::Shutdown : VmTerminationReason::Crashed;
        m_terminationInformation.Details = std::format(L"OpenVMM process exited with code {}", ExitCode);
        CloseGuestListenersLocked(m_description.Identity);
    }
    if (ExitCode != ERROR_SUCCESS)
    {
        LOG_IF_WIN32_BOOL_FALSE(SetEvent(m_crashEvent.get()));
    }
    LOG_IF_WIN32_BOOL_FALSE(SetEvent(m_exitEvent.get()));
    NotifyTerminated(m_description.Identity);
}

VmPlatformCapabilities OpenVmmVirtualMachineBackend::QueryCapabilities()
{
    VmPlatformCapabilities capabilities;
    capabilities.Backend = BackendKind::OpenVmm;
    for (const auto feature :
         {VmFeature::SerialConsole,
          VmFeature::HighMmio,
          VmFeature::PersistentMemory,
          VmFeature::VirtioFsFileBacked,
          VmFeature::UserModeNatNetwork,
          VmFeature::TcpPortBinding,
          VmFeature::UdpPortBinding,
          VmFeature::Ipv6PortBinding,
          VmFeature::ScopedIpv6PortBinding})
    {
        capabilities.Features.set(static_cast<size_t>(feature));
    }
    return capabilities;
}

VmPlatformCapabilities OpenVmmVirtualMachineBackend::GetCapabilities() const
{
    return QueryCapabilities();
}

VmDescription OpenVmmVirtualMachineBackend::GetDescription() const
{
    return m_description;
}

VmState OpenVmmVirtualMachineBackend::GetState() const
{
    auto lock = m_lock.lock_shared();
    return m_state;
}

VmTerminationInformation OpenVmmVirtualMachineBackend::GetTerminationReason() const
{
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_exitEvent.is_signaled());
    auto lock = m_lock.lock_shared();
    return m_terminationInformation;
}

wil::unique_handle OpenVmmVirtualMachineBackend::GetTerminationEvent() const
{
    return wil::unique_handle{wsl::windows::common::wslutil::DuplicateHandle(m_exitEvent.get())};
}

wil::unique_handle OpenVmmVirtualMachineBackend::GetCrashEvent() const
{
    return wil::unique_handle{wsl::windows::common::wslutil::DuplicateHandle(m_crashEvent.get())};
}

std::optional<std::filesystem::path> OpenVmmVirtualMachineBackend::GetCrashLogPath() const
{
    return std::nullopt;
}

void OpenVmmVirtualMachineBackend::Start()
{
    const auto startTimeMs = GetTickCount64();
    WSL_LOG("OpenVmmStartVmBegin", TraceLoggingValue(m_description.Identity.VmId, "vmId"));
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    const auto result = WslOpenVmmVmResume(m_vm.get());
    WSL_LOG(
        "OpenVmmStartVmEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingHResult(result, "result"),
        TraceLoggingValue(GetTickCount64() - startTimeMs, "durationMs"));
    THROW_IF_FAILED(result);
    m_state = VmState::Running;
}

void OpenVmmVirtualMachineBackend::Terminate()
{
    const auto startTimeMs = GetTickCount64();
    WSL_LOG("OpenVmmTerminateVmBegin", TraceLoggingValue(m_description.Identity.VmId, "vmId"));
    auto persistentMemoryLock = m_persistentMemoryLock.lock_exclusive();
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    const auto teardownResult = WslOpenVmmVmTeardown(m_vm.get());
    WSL_LOG(
        "OpenVmmTeardownVm",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingHResult(teardownResult, "result"));
    const auto quitResult = WslOpenVmmVmQuit(m_vm.get());
    const auto waitResult = WaitForSingleObject(m_process.get(), c_rpcTimeoutMs);
    const auto waitError = waitResult == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    WSL_LOG(
        "OpenVmmQuitVm",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingHResult(quitResult, "result"),
        TraceLoggingValue(waitResult, "waitResult"),
        TraceLoggingValue(waitError, "waitError"),
        TraceLoggingValue(GetTickCount64() - startTimeMs, "durationMs"));
    THROW_IF_WIN32_ERROR(waitError);
    if (waitResult != WAIT_OBJECT_0)
    {
        THROW_IF_FAILED(quitResult);
        THROW_IF_FAILED(teardownResult);
        THROW_HR(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
    }

    m_vm.reset();
    m_state = VmState::Stopped;
    m_attachedDisks.clear();
    m_diskInstanceIds.clear();
    m_persistentMemoryDevices.clear();
    m_fileSystemShares.clear();
    m_fileSystemDevices.clear();
    m_portBindings.clear();
    m_networkAdapters.clear();
    CloseGuestListenersLocked(m_description.Identity);
    WSL_LOG(
        "OpenVmmTerminateVmEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(GetTickCount64() - startTimeMs, "durationMs"));
}

std::shared_ptr<VmGuestListenerState> OpenVmmVirtualMachineBackend::ConfigureGuestListener(const VmGuestListener& Listener)
{
    WSL_LOG(
        "OpenVmmCreateGuestListenerBegin",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Listener.Port.Value, "port"));

    auto listener = std::make_shared<GuestListener>();
    listener->Listener = Listener;
    const auto path = GetVsockListenerPath(m_fileSystemResources.VsockPath, Listener.Port);
    DeleteOwnedFile(path);

    listener->Socket.reset(::socket(AF_UNIX, SOCK_STREAM, 0));
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), !listener->Socket);

    const auto address = wsl::windows::common::vsock::GetUnixSocketAddress(path);

    THROW_WIN32_IF(
        static_cast<DWORD>(WSAGetLastError()),
        bind(listener->Socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR);
    auto bindCleanup = wil::scope_exit([&] {
        listener->Socket.reset();
        DeleteOwnedFile(path);
    });
    listener->SocketFile.reset(CreateFileW(
        path.c_str(),
        DELETE | GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    THROW_LAST_ERROR_IF(!listener->SocketFile);
    bindCleanup.release();
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), listen(listener->Socket.get(), SOMAXCONN) == SOCKET_ERROR);

    WSL_LOG(
        "OpenVmmCreateGuestListenerEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(listener->Listener.Id.Value, "listenerId"),
        TraceLoggingValue(Listener.Port.Value, "port"));
    return listener;
}

VmGuestListener OpenVmmVirtualMachineBackend::CreateGuestListener(GuestServicePort Port)
{
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    return RegisterGuestListenerLocked(m_description.Identity, Port);
}

wil::unique_socket OpenVmmVirtualMachineBackend::ConnectGuest(GuestServicePort Port, _In_opt_ HANDLE ExitHandle)
{
    const auto startTimeMs = GetTickCount64();
    WSL_LOG(
        "OpenVmmConnectGuestBegin", TraceLoggingValue(m_description.Identity.VmId, "vmId"), TraceLoggingValue(Port.Value, "port"));
    {
        auto lock = m_lock.lock_shared();
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
        THROW_HR_IF(E_ABORT, m_exitEvent.is_signaled());
        THROW_HR_IF(E_ABORT, ExitHandle && WaitForSingleObject(ExitHandle, 0) == WAIT_OBJECT_0);
    }

    const auto cancellationEvent = ExitHandle ? ExitHandle : m_exitEvent.get();
    auto socket = GetGuestConnector().Connect(Port.Value, cancellationEvent);

    WSL_LOG(
        "OpenVmmConnectGuestEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Port.Value, "port"),
        TraceLoggingValue(GetTickCount64() - startTimeMs, "durationMs"));
    return socket;
}

void OpenVmmVirtualMachineBackend::CloseGuestListener(VmListenerId Listener)
{
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    RemoveGuestListenerLocked(Listener, m_description.Identity);
}

VmDiskAttachment OpenVmmVirtualMachineBackend::AttachDisk(const VmDiskRequest& Request)
{
    const auto& source = validation::ValidateDiskRequest(Request);
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);

    // WSL attaches a distribution's VHD every time an instance is created and only detaches it when the
    // disk is ejected, so the same path can be attached while a previous attachment is still present. HCS
    // tolerates this because it shares the underlying virtual disk object between attachments, but OpenVMM
    // opens the backing file exclusively and a second open fails with a sharing violation. Reuse the
    // existing attachment so the guest keeps a single block device for the disk.
    const auto existing = std::find_if(m_attachedDisks.begin(), m_attachedDisks.end(), [&](const auto& entry) {
        return entry.second.ReadOnly == Request.ReadOnly &&
               wsl::windows::common::string::IsPathComponentEqual(entry.second.Path, source.Path.native());
    });
    if (!Request.Placement && existing != m_attachedDisks.end())
    {
        WSL_LOG(
            "OpenVmmAttachDiskReused",
            TraceLoggingValue(m_description.Identity.VmId, "vmId"),
            TraceLoggingValue(existing->second.Id.Value, "diskId"),
            TraceLoggingValue(existing->second.GuestAddress.Lun, "lun"));
        return existing->second;
    }

    const auto lunInUse = [this](std::uint32_t Lun) {
        for (const auto& entry : m_attachedDisks)
        {
            if (entry.second.GuestAddress.Lun == Lun)
            {
                return true;
            }
        }
        return false;
    };

    std::uint32_t lun = 0;
    if (Request.Placement)
    {
        const auto& address = Request.Placement->Address;
        THROW_HR_IF(c_notSupported, address.Controller != 0);
        THROW_HR_IF(E_BOUNDS, address.Lun >= c_maximumDisks);
        lun = address.Lun;
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), lunInUse(lun));
    }
    else
    {
        while (lun < c_maximumDisks && lunInUse(lun))
        {
            ++lun;
        }
        THROW_HR_IF(WSL_E_TOO_MANY_DISKS_ATTACHED, lun == c_maximumDisks);
    }

    THROW_HR_IF(E_BOUNDS, m_nextDiskId == UINT64_MAX);
    const VmDiskAttachment attachment{
        {m_description.Identity, m_nextDiskId},
        {0, lun},
        Request.BootCritical ? VmDiskTransport::Scsi : VmDiskTransport::VirtioBlk,
        Request.ReadOnly,
        Request.UserDisk,
        source.Path.native(),
        false};
    GUID instanceId{};
    HRESULT result;
    if (Request.BootCritical)
    {
        result = WslOpenVmmVmAttachScsiDisk(
            m_vm.get(), attachment.GuestAddress.Controller, attachment.GuestAddress.Lun, source.Path.c_str(), Request.ReadOnly);
    }
    else
    {
        THROW_IF_FAILED(CoCreateGuid(&instanceId));
        const auto instanceIdString = wsl::shared::string::GuidToString<wchar_t>(instanceId, wsl::shared::string::GuidToStringFlags::None);
        result = WslOpenVmmVmAttachVirtioBlk(
            m_vm.get(), instanceIdString.c_str(), attachment.GuestAddress.Lun, source.Path.c_str(), Request.ReadOnly);
    }
    WSL_LOG(
        "OpenVmmAttachDiskEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(attachment.Id.Value, "diskId"),
        TraceLoggingValue(attachment.GuestAddress.Controller, "controller"),
        TraceLoggingValue(attachment.GuestAddress.Lun, "lun"),
        TraceLoggingValue(Request.ReadOnly, "readOnly"),
        TraceLoggingHResult(result, "result"));
    THROW_IF_FAILED(result);
    const auto [disk, inserted] = m_attachedDisks.emplace(attachment.Id.Value, attachment);
    WI_ASSERT(inserted);
    if (!Request.BootCritical)
    {
        const auto [instance, instanceInserted] = m_diskInstanceIds.emplace(attachment.Id.Value, instanceId);
        WI_ASSERT(instanceInserted);
    }
    ++m_nextDiskId;
    return attachment;
}

std::vector<VmDiskAttachment> OpenVmmVirtualMachineBackend::GetAttachedDisks() const
{
    auto lock = m_lock.lock_shared();
    std::vector<VmDiskAttachment> disks;
    disks.reserve(m_attachedDisks.size());
    for (const auto& entry : m_attachedDisks)
    {
        disks.push_back(entry.second);
    }
    return disks;
}

void OpenVmmVirtualMachineBackend::DetachDisk(VmDiskId Disk)
{
    ExecutionContext context(Context::DetachDisk);
    WSL_LOG(
        "OpenVmmDetachDiskBegin", TraceLoggingValue(m_description.Identity.VmId, "vmId"), TraceLoggingValue(Disk.Value, "diskId"));
    ValidateResourceId(Disk, m_description.Identity);
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    const auto disk = m_attachedDisks.find(Disk.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), disk == m_attachedDisks.end());
    const auto instance = m_diskInstanceIds.find(Disk.Value);
    if (instance == m_diskInstanceIds.end())
    {
        THROW_IF_FAILED(WslOpenVmmVmDetachScsiDisk(m_vm.get(), disk->second.GuestAddress.Controller, disk->second.GuestAddress.Lun));
    }
    else
    {
        const auto instanceId = wsl::shared::string::GuidToString<wchar_t>(instance->second, wsl::shared::string::GuidToStringFlags::None);
        THROW_IF_FAILED(WslOpenVmmVmDetachVpciDevice(m_vm.get(), instanceId.c_str()));
        m_diskInstanceIds.erase(instance);
    }
    WSL_LOG(
        "OpenVmmDetachDiskEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Disk.Value, "diskId"),
        TraceLoggingValue(disk->second.GuestAddress.Controller, "controller"),
        TraceLoggingValue(disk->second.GuestAddress.Lun, "lun"));
    m_attachedDisks.erase(disk);
}

VmPersistentMemoryDevice OpenVmmVirtualMachineBackend::AddPersistentMemory(const VmPersistentMemoryRequest& Request)
{
    THROW_HR_IF_MSG(
        c_notSupported,
        Request.UserToken.has_value(),
        "OpenVMM persistent memory uses the VM owner identity and does not support alternate user tokens");

    auto persistentMemoryLock = m_persistentMemoryLock.lock_exclusive();
    VmPersistentMemoryDevice device{};
    {
        auto lock = m_lock.lock_exclusive();
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm || m_state == VmState::Stopped);
        THROW_HR_IF(E_BOUNDS, m_nextDeviceId == UINT64_MAX);
        THROW_HR_IF(E_BOUNDS, m_nextPersistentMemoryIndex == UINT32_MAX);
        THROW_IF_FAILED(CoCreateGuid(&device.GuestInstanceId));
        device.Id = {m_description.Identity, m_nextDeviceId};
        device.Index = m_nextPersistentMemoryIndex;
        device.EffectiveHostPath = Request.Path;
        device.ReadOnly = Request.ReadOnly;

        const auto instanceId =
            wsl::shared::string::GuidToString<wchar_t>(device.GuestInstanceId, wsl::shared::string::GuidToStringFlags::None);
        const auto result = WslOpenVmmVmAddPmem(m_vm.get(), instanceId.c_str(), Request.Path.c_str(), Request.ReadOnly);
        WSL_LOG(
            "OpenVmmAddPersistentMemory",
            TraceLoggingValue(m_description.Identity.VmId, "vmId"),
            TraceLoggingValue(device.Id.Value, "deviceId"),
            TraceLoggingValue(device.Index, "index"),
            TraceLoggingValue(Request.ReadOnly, "readOnly"),
            TraceLoggingHResult(result, "result"));
        THROW_IF_FAILED(result);

        const auto [entry, inserted] = m_persistentMemoryDevices.emplace(device.Id.Value, device);
        WI_ASSERT(inserted);
        ++m_nextDeviceId;
        ++m_nextPersistentMemoryIndex;
    }

    if (Request.WaitForGuestDevice)
    {
        Request.WaitForGuestDevice(device.Index);
    }
    return device;
}

VmGpuAttachment OpenVmmVirtualMachineBackend::AddGpu(const VmGpuRequest&)
{
    THROW_HR_MSG(c_notSupported, "OpenVMM does not support GPU assignment");
}

VmFileSystemDevice OpenVmmVirtualMachineBackend::CreateFileSystemDevice(const VmFileSystemDeviceRequest& Request)
{
    THROW_HR_IF_MSG(c_notSupported, Request.UserToken.has_value(), "OpenVMM does not support a per-device user token");
    const auto* transport = std::get_if<VmVirtioFsDevice>(&Request.Transport);
    WSL_LOG(
        "OpenVmmCreateFileSystemDeviceBegin",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(transport ? transport->Tag.c_str() : L"", "tag"));
    THROW_HR_IF(c_notSupported, !transport);
    THROW_HR_IF_MSG(
        c_notSupported,
        transport->Layout != VmVirtioFsLayout::SingleShare,
        "OpenVMM currently supports only single-share virtio-fs devices");
    THROW_HR_IF_MSG(
        c_notSupported,
        !transport->Options.MountOptions.empty(),
        "A single-share OpenVMM virtio-fs device accepts mount options on its share");
    THROW_HR_IF_MSG(
        c_notSupported, !!transport->Options.UserToken, "OpenVMM does not support serving a virtio-fs device under a user token");

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    THROW_HR_IF(E_BOUNDS, m_nextDeviceId == UINT64_MAX);
    for (const auto& entry : m_fileSystemDevices)
    {
        THROW_HR_IF(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
            wsl::shared::string::IsEqual(std::get<VmVirtioFsDevice>(entry.second.Transport).Tag, transport->Tag, false));
    }

    VmFileSystemDevice device{
        {m_description.Identity, m_nextDeviceId},
        VmFileSystemDeviceState::Prepared,
        {},
        Request.Transport,
        wsl::windows::common::security::IsTokenElevated(m_description.Identity.UserToken.get())};
    const auto inserted = m_fileSystemDevices.emplace(device.Id.Value, FileSystemDevice{device, {}}).second;
    WI_ASSERT(inserted);
    ++m_nextDeviceId;
    WSL_LOG(
        "OpenVmmCreateFileSystemDeviceEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(device.Id.Value, "deviceId"),
        TraceLoggingValue(transport->Tag.c_str(), "tag"));
    return device;
}

std::optional<VmFileSystemDevice> OpenVmmVirtualMachineBackend::GetFileSystemDevice(const VmFileSystemDevicePredicate& Predicate) const
{
    auto lock = m_lock.lock_shared();
    for (const auto& entry : m_fileSystemDevices)
    {
        if (Predicate(entry.second))
        {
            return entry.second;
        }
    }
    return {};
}

VmFileSystemDevice OpenVmmVirtualMachineBackend::GetFileSystemDeviceStatus(VmDeviceId Device)
{
    validation::ValidateResourceId(Device, m_description.Identity);
    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    const auto device = m_fileSystemDevices.find(Device.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), device == m_fileSystemDevices.end());
    return device->second;
}

VmFileSystemShare OpenVmmVirtualMachineBackend::AddFileSystemShare(VmDeviceId Device, const VmFileSystemShareRequest& Request)
{
    WSL_LOG(
        "OpenVmmAddFileSystemShareBegin",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(Request.ReadOnly, "readOnly"));
    ValidateResourceId(Device, m_description.Identity);
    THROW_HR_IF_MSG(
        E_INVALIDARG, !Request.Name.empty(), "A single-share OpenVMM virtio-fs device does not accept a child share name");
    const auto* options = std::get_if<VmVirtioFsShareOptions>(&Request.Options);
    THROW_HR_IF_MSG(c_notSupported, !options, "OpenVMM supports only virtio-fs share options");
    THROW_HR_IF_MSG(c_notSupported, !!options->UserToken, "OpenVMM does not support serving a virtio-fs share under a user token");
    std::filesystem::path hostPath;
    {
        const auto runAsUser = wil::impersonate_token(m_description.Identity.UserToken.get());
        hostPath = wsl::windows::common::filesystem::GetCanonicalPath(Request.HostPath);
        const auto attributes = GetFileAttributesW(hostPath.c_str());
        THROW_LAST_ERROR_IF(attributes == INVALID_FILE_ATTRIBUTES);
        THROW_HR_IF_MSG(
            E_INVALIDARG, WI_IsFlagClear(attributes, FILE_ATTRIBUTE_DIRECTORY), "The virtio-fs host path must be a directory");
    }

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    const auto device = m_fileSystemDevices.find(Device.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), device == m_fileSystemDevices.end());
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), device->second.Backend.Share.has_value());
    THROW_HR_IF(E_BOUNDS, m_nextShareId == UINT64_MAX);

    VmFileSystemShare share{
        {m_description.Identity, m_nextShareId},
        Device,
        VmVirtioFsShareAddress{std::get<VmVirtioFsDevice>(device->second.Transport).Tag, {}},
        hostPath,
        Request.ReadOnly,
        options->MountOptions,
        device->second.Elevated};
    const auto& guestAddress = std::get<VmVirtioFsShareAddress>(share.GuestAddress);
    const auto [entry, inserted] = m_fileSystemShares.emplace(share.Id.Value, share);
    WI_ASSERT(inserted);
    device->second.Backend.Share = share.Id.Value;
    device->second.State = VmFileSystemDeviceState::Serving;
    auto rollback = wil::scope_exit([this, &device, &entry] {
        device->second.Backend.Share.reset();
        device->second.State = VmFileSystemDeviceState::Prepared;
        m_fileSystemShares.erase(entry);
    });
    const auto mountOptions = FormatVirtioFsMountOptions(options->MountOptions);
    const auto result =
        WslOpenVmmVmAddShare(m_vm.get(), guestAddress.Tag.c_str(), hostPath.c_str(), Request.ReadOnly, mountOptions.c_str());
    WSL_LOG(
        "OpenVmmAddFileSystemShareEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(share.Id.Value, "shareId"),
        TraceLoggingValue(guestAddress.Tag.c_str(), "tag"),
        TraceLoggingHResult(result, "result"));
    THROW_IF_FAILED(result);
    ++m_nextShareId;
    rollback.release();
    return share;
}

std::optional<VmFileSystemShare> OpenVmmVirtualMachineBackend::GetFileSystemShare(const VmFileSystemSharePredicate& Predicate) const
{
    auto lock = m_lock.lock_shared();
    for (const auto& entry : m_fileSystemShares)
    {
        if (Predicate(entry.second))
        {
            return entry.second;
        }
    }
    return {};
}

void OpenVmmVirtualMachineBackend::RemoveFileSystemShare(VmShareId Share)
{
    WSL_LOG(
        "OpenVmmRemoveFileSystemShareBegin",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Share.Value, "shareId"));
    ValidateResourceId(Share, m_description.Identity);
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    const auto share = m_fileSystemShares.find(Share.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), share == m_fileSystemShares.end());
    const auto device = m_fileSystemDevices.find(share->second.Device.Value);
    THROW_HR_IF(E_UNEXPECTED, device == m_fileSystemDevices.end() || device->second.Backend.Share != Share.Value);

    const auto& guestAddress = std::get<VmVirtioFsShareAddress>(share->second.GuestAddress);
    const auto result = WslOpenVmmVmRemoveShare(m_vm.get(), guestAddress.Tag.c_str());
    WSL_LOG(
        "OpenVmmRemoveFileSystemShareEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Share.Value, "shareId"),
        TraceLoggingValue(device->second.Id.Value, "deviceId"),
        TraceLoggingValue(guestAddress.Tag.c_str(), "tag"),
        TraceLoggingHResult(result, "result"));
    THROW_IF_FAILED(result);
    device->second.Backend.Share.reset();
    device->second.State = VmFileSystemDeviceState::Prepared;
    m_fileSystemShares.erase(share);
}

VmSharedMemoryDevice OpenVmmVirtualMachineBackend::AddSharedMemory(const VmSharedMemoryRequest&)
{
    THROW_HR_MSG(c_notSupported, "OpenVMM does not support section-backed shared memory devices");
}

void OpenVmmVirtualMachineBackend::ConfigureGuestDma(const VmGuestDmaRequest&)
{
    THROW_HR_MSG(c_notSupported, "OpenVMM does not support configuring a guest DMA window");
}

void OpenVmmVirtualMachineBackend::RemoveDevice(VmDeviceId Device)
{
    validation::ValidateResourceId(Device, m_description.Identity);
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    const auto device = m_fileSystemDevices.find(Device.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), device == m_fileSystemDevices.end());
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_BUSY), device->second.Backend.Share.has_value());
    m_fileSystemDevices.erase(device);
}

VmNetworkAttachment OpenVmmVirtualMachineBackend::AddNetworkAdapter(const VmNetworkAdapterRequest& Request)
{
    ExecutionContext context(Context::ConfigureNetworking);
    THROW_HR_MSG(c_notSupported, "OpenVMM network adapter '%ls' must be configured at VM creation time", Request.Tag.c_str());
}

void OpenVmmVirtualMachineBackend::UpdateNetworkAdapter(VmDeviceId Device, const VmNetworkConfiguration&)
{
    ExecutionContext context(Context::ConfigureNetworking);
    validation::ValidateResourceId(Device, m_description.Identity);
    THROW_HR_MSG(c_notSupported, "OpenVMM network adapters cannot be updated after VM creation");
}

void OpenVmmVirtualMachineBackend::RemoveNetworkAdapter(VmDeviceId Device)
{
    ExecutionContext context(Context::ConfigureNetworking);
    validation::ValidateResourceId(Device, m_description.Identity);
    THROW_HR_MSG(c_notSupported, "OpenVMM network adapters cannot be removed after VM creation");
}

VmPortBinding OpenVmmVirtualMachineBackend::BindPort(VmDeviceId Device, const VmPortBindingRequest& Request)
{
    ExecutionContext context(Context::ConfigureNetworking);
    WSL_LOG(
        "OpenVmmBindPortBegin",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(static_cast<UINT32>(Request.Protocol), "protocol"),
        TraceLoggingValue(Request.HostPort, "hostPort"),
        TraceLoggingValue(Request.GuestPort, "guestPort"));
    ValidateResourceId(Device, m_description.Identity);
    THROW_HR_IF(E_INVALIDARG, Request.ListenAddress.family != IpAddressFamily_V4 && Request.ListenAddress.family != IpAddressFamily_V6);
    THROW_HR_IF_MSG(
        c_notSupported, Request.HostPort == 0, "OpenVMM cannot report the allocated port for a dynamic host port binding");
    THROW_HR_IF(E_INVALIDARG, Request.GuestPort == 0);
    const auto hostAddress = wsl::windows::common::string::IpAddressToWstring(Request.ListenAddress, Request.ListenScopeId);
    bool tcp = false;
    switch (Request.Protocol)
    {
    case TransportProtocol_Tcp:
        tcp = true;
        break;
    case TransportProtocol_Udp:
        break;
    default:
        THROW_HR(E_INVALIDARG);
    }

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    const auto adapter = m_networkAdapters.find(Device.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), adapter == m_networkAdapters.end());
    THROW_HR_IF(E_BOUNDS, m_nextPortBindingId == UINT64_MAX);

    VmPortBinding binding{
        {m_description.Identity, m_nextPortBindingId},
        Device,
        Request.Protocol,
        Request.ListenAddress,
        Request.ListenScopeId,
        Request.HostPort,
        Request.GuestPort};
    const auto [entry, inserted] =
        m_portBindings.emplace(binding.Id.Value, PortBinding{binding, {adapter->second.Backend.NicId, hostAddress}});
    WI_ASSERT(inserted);
    auto rollback = wil::scope_exit([this, &entry] { m_portBindings.erase(entry); });
    const auto result = WslOpenVmmVmBindPort(
        m_vm.get(), adapter->second.Backend.NicId.c_str(), Request.HostPort, Request.GuestPort, tcp, hostAddress.c_str());
    WSL_LOG(
        "OpenVmmBindPortEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(binding.Id.Value, "bindingId"),
        TraceLoggingHResult(result, "result"));
    THROW_IF_FAILED(result);
    ++m_nextPortBindingId;
    rollback.release();
    return binding;
}

void OpenVmmVirtualMachineBackend::UnbindPort(VmPortBindingId Binding)
{
    ExecutionContext context(Context::ConfigureNetworking);
    WSL_LOG(
        "OpenVmmUnbindPortBegin",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Binding.Value, "bindingId"));
    ValidateResourceId(Binding, m_description.Identity);
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    const auto binding = m_portBindings.find(Binding.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), binding == m_portBindings.end());

    const auto result = WslOpenVmmVmUnbindPort(
        m_vm.get(),
        binding->second.Backend.NicId.c_str(),
        binding->second.EffectiveHostPort,
        binding->second.GuestPort,
        binding->second.Protocol == TransportProtocol_Tcp,
        binding->second.Backend.HostAddress.c_str());
    WSL_LOG(
        "OpenVmmUnbindPortEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Binding.Value, "bindingId"),
        TraceLoggingValue(binding->second.Device.Value, "deviceId"),
        TraceLoggingHResult(result, "result"));
    THROW_IF_FAILED(result);
    m_portBindings.erase(binding);
}

IpAddress OpenVmmVirtualMachineBackend::CreateVirtualAddress(VmDeviceId Device, const IpAddress& Destination)
{
    ExecutionContext context(Context::ConfigureNetworking);
    validation::ValidateResourceId(Device, m_description.Identity);
    THROW_HR_IF(E_INVALIDARG, Destination.family != IpAddressFamily_V4 && Destination.family != IpAddressFamily_V6);
    const auto destination = wsl::windows::common::string::IpAddressToWstring(Destination);
    WSL_LOG(
        "OpenVmmCreateVirtualAddressBegin",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(destination.c_str(), "destination"));

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    const auto adapter = m_networkAdapters.find(Device.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), adapter == m_networkAdapters.end());

    // INET6_ADDRSTRLEN is the documented minimum buffer size of the export.
    wchar_t virtualAddress[INET6_ADDRSTRLEN]{};
    const auto result = WslOpenVmmVmCreateVirtualAddress(
        m_vm.get(), adapter->second.Backend.NicId.c_str(), destination.c_str(), virtualAddress, ARRAYSIZE(virtualAddress));
    WSL_LOG(
        "OpenVmmCreateVirtualAddressEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(virtualAddress, "virtualAddress"),
        TraceLoggingHResult(result, "result"));
    THROW_IF_FAILED(result);

    const auto address = wsl::windows::common::string::StringToSockAddrInet(virtualAddress);
    IpAddress converted{};
    if (address.si_family == AF_INET)
    {
        converted.family = IpAddressFamily_V4;
        std::memcpy(converted.bytes, &address.Ipv4.sin_addr, sizeof(address.Ipv4.sin_addr));
    }
    else
    {
        THROW_HR_IF(E_UNEXPECTED, address.si_family != AF_INET6);
        converted.family = IpAddressFamily_V6;
        std::memcpy(converted.bytes, &address.Ipv6.sin6_addr, sizeof(address.Ipv6.sin6_addr));
    }

    return converted;
}

void OpenVmmVirtualMachineBackend::CreateDnsRecord(VmDeviceId Device, const VmDnsRecord& Record)
{
    ExecutionContext context(Context::ConfigureNetworking);
    validation::ValidateResourceId(Device, m_description.Identity);
    THROW_HR_IF_MSG(E_INVALIDARG, Record.Name.empty(), "A DNS record requires a name");
    THROW_HR_IF_MSG(c_notSupported, Record.Type != DnsRecordType_A, "Only A records are supported");
    THROW_HR_IF(E_INVALIDARG, Record.Address.family != IpAddressFamily_V4);
    const auto name = wsl::shared::string::MultiByteToWide(Record.Name);
    const auto address = wsl::windows::common::string::IpAddressToWstring(Record.Address);
    WSL_LOG(
        "OpenVmmCreateDnsRecordBegin",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(Record.Name.c_str(), "name"),
        TraceLoggingValue(address.c_str(), "address"));

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    const auto adapter = m_networkAdapters.find(Device.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), adapter == m_networkAdapters.end());

    const auto result = WslOpenVmmVmAddDnsRecord(m_vm.get(), adapter->second.Backend.NicId.c_str(), name.c_str(), address.c_str());
    WSL_LOG(
        "OpenVmmCreateDnsRecordEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(Record.Name.c_str(), "name"),
        TraceLoggingHResult(result, "result"));
    THROW_IF_FAILED(result);
}
