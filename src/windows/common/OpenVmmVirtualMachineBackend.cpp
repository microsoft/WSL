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
#include "HandleIO.h"
#include "SubProcess.h"
#include "wslopenvmm.h"

using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;

namespace {

constexpr UINT64 c_mib = 1024 * 1024;
constexpr UINT32 c_maximumDisks = 254;
constexpr UINT32 c_rpcTimeoutMs = 30000;
constexpr HRESULT c_notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

void DestroyConfig(WslOpenVmmConfig* Config) noexcept
{
    WslOpenVmmDestroyConfig(&Config);
}

using UniqueConfig = wil::unique_any<WslOpenVmmConfig*, decltype(&DestroyConfig), DestroyConfig>;

void ValidateFeature(VmFeatureRequest Request, PCWSTR Setting)
{
    switch (Request)
    {
    case VmFeatureRequest::Disabled:
    case VmFeatureRequest::Preferred:
        return;
    case VmFeatureRequest::Required:
        THROW_HR_MSG(c_notSupported, "OpenVMM does not support the required %ls setting", Setting);
    }

    THROW_HR(E_INVALIDARG);
}

const VmVirtualDiskSource& GetVirtualDiskSource(const VmDiskRequest& Request)
{
    const auto* source = std::get_if<VmVirtualDiskSource>(&Request.Source);
    THROW_HR_IF(c_notSupported, source == nullptr);
    return *source;
}

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

std::filesystem::path GetVsockListenerPath(const std::filesystem::path& VsockPath, GuestServicePort Port)
{
    return std::format(L"{}_{:08x}-facb-11e6-bd58-64006a7986d3", VsockPath.native(), Port.Value);
}

SOCKADDR_UN GetUnixSocketAddress(const std::filesystem::path& Path)
{
    SOCKADDR_UN address{};
    address.sun_family = AF_UNIX;
    const auto narrowPath = wsl::shared::string::WideToMultiByte(Path.native());
    THROW_HR_IF_MSG(E_INVALIDARG, narrowPath.size() >= sizeof(address.sun_path), "vsock bridge path too long: %hs", narrowPath.c_str());
    std::copy(narrowPath.cbegin(), narrowPath.cend(), address.sun_path);
    address.sun_path[narrowPath.size()] = '\0';
    return address;
}

std::wstring FormatIpAddress(const VmIpAddress& Address)
{
    if (const auto* ipv4 = std::get_if<VmIpv4Address>(&Address))
    {
        return std::format(L"{}.{}.{}.{}", ipv4->Bytes[0], ipv4->Bytes[1], ipv4->Bytes[2], ipv4->Bytes[3]);
    }

    const auto& ipv6 = std::get<VmIpv6Address>(Address);
    IN6_ADDR address{};
    std::copy(ipv6.Bytes.begin(), ipv6.Bytes.end(), address.u.Byte);
    std::wstring result(INET6_ADDRSTRLEN, L'\0');
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), InetNtopW(AF_INET6, &address, result.data(), result.size()) == nullptr);
    result.resize(std::wcslen(result.c_str()));
    if (ipv6.ScopeId != 0)
    {
        result += std::format(L"%{}", ipv6.ScopeId);
    }

    return result;
}

template <typename Tag>
void ValidateResourceId(const VmResourceId<Tag>& Id, const VmInstanceId& Owner)
{
    THROW_HR_IF(E_INVALIDARG, Id.Value == 0 || !IsEqualGUID(Id.Owner.VmId, Owner.VmId));
}

} // namespace

OpenVmmVirtualMachineBackend::GuestListener::~GuestListener() noexcept
{
    Socket.reset();
    DeleteOwnedFile(Path);
}

void OpenVmmVirtualMachineBackend::CloseGuestListeners() noexcept
{
    WSL_LOG(
        "OpenVmmCloseGuestListeners",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(m_guestListeners.size(), "listenerCount"));
    for (const auto& entry : m_guestListeners)
    {
        LOG_IF_WIN32_BOOL_FALSE(SetEvent(entry.second->CancellationEvent.get()));
    }

    m_guestListeners.clear();
}

VmDescription wsl::windows::common::vm::openvmm::ValidateCreateRequest(const VmCreateRequest& Request)
{
    THROW_HR_IF_MSG(c_notSupported, wsl::shared::Arm64, "OpenVMM direct boot is currently supported only on x64");
    THROW_HR_IF(c_notSupported, Request.Boot.Method == VmBootMethod::Uefi);
    THROW_HR_IF(c_notSupported, Request.Boot.Method != VmBootMethod::Automatic && Request.Boot.Method != VmBootMethod::LinuxDirect);
    THROW_HR_IF(c_notSupported, Request.Boot.RequestedDmaBounceBufferBytes.has_value());

    VmDescription description;
    description.Identity.VmId = Request.VmId;
    description.Backend = BackendKind::OpenVmm;
    description.Processor.Count = Request.Processor.Count;
    description.Memory.SizeBytes = (Request.Memory.SizeBytes / c_mib) * c_mib;
    ValidateFeature(Request.Processor.NestedVirtualization, L"nested virtualization");
    ValidateFeature(Request.Processor.PerfmonPmu, L"PMU");
    ValidateFeature(Request.Processor.PerfmonLbr, L"LBR");
    ValidateFeature(Request.Memory.AllowOvercommit, L"memory overcommit");
    ValidateFeature(Request.Memory.DeferredCommit, L"deferred memory commit");
    ValidateFeature(Request.Memory.ColdDiscard, L"cold discard");

    if (Request.CrashCapture)
    {
        THROW_HR_IF(c_notSupported, Request.CrashCapture->Policy == VmSelectionPolicy::Required);
    }

    description.Boot.Method = VmBootMethod::LinuxDirect;
    description.Boot.KernelCommandLine = Request.Boot.GuestCommandLine;
    if (!Request.Boot.UserCommandLine.empty())
    {
        if (!description.Boot.KernelCommandLine.empty())
        {
            description.Boot.KernelCommandLine += L" ";
        }
        description.Boot.KernelCommandLine += Request.Boot.UserCommandLine;
    }

    for (const auto& console : Request.Consoles)
    {
        if (!std::holds_alternative<VmSerialConsole>(console.Device))
        {
            const auto& virtio = std::get<VmVirtioConsole>(console.Device);
            THROW_HR_IF(c_notSupported, virtio.Port != 0 || !virtio.GuestName.empty());
        }
        description.Boot.Consoles.push_back(console);
    }

    std::bitset<c_maximumDisks> allocated;
    for (const auto& disk : Request.BootDisks)
    {
        THROW_HR_IF(E_INVALIDARG, disk.Key.empty() || description.BootDisks.contains(disk.Key));
        description.BootDisks.emplace(disk.Key, VmDiskAttachment{});
        GetVirtualDiskSource(disk.Disk);
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
        description.BootDisks.at(disk.Key) = {{description.Identity, nextId++}, {0, lun}, disk.Disk.ReadOnly};
    }

    // OpenVMM's port RPC channel targets only the first Consomme NIC.
    THROW_HR_IF_MSG(c_notSupported, Request.NetworkAdapters.size() > 1, "OpenVMM supports one creation-time network adapter");
    for (const auto& adapter : Request.NetworkAdapters)
    {
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
        CloseGuestListeners();
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
    ExecutionContext context(Context::CreateVm);
    const auto startTimeMs = GetTickCount64();
    WSL_LOG(
        "OpenVmmCreateVmBegin",
        TraceLoggingValue(Request.VmId, "vmId"),
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
            TraceLoggingValue(Request.VmId, "vmId"),
            TraceLoggingValue(GetTickCount64() - startTimeMs, "durationMs"));
    }
    catch (...)
    {
        const auto result = wil::ResultFromCaughtException();
        WSL_LOG(
            "OpenVmmCreateVmFailed",
            TraceLoggingValue(Request.VmId, "vmId"),
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

    auto id = wsl::shared::string::GuidToString<wchar_t>(Request.VmId, wsl::shared::string::GuidToStringFlags::None);
    std::erase(id, L'-');
    // An exclusive directory creation prevents shortened path IDs from aliasing another VM.
    m_fileSystemResources.SocketDirectory = filesystem::GetTempFolderPath(GetCurrentProcessToken()) / (L"ov-" + id.substr(0, 16));
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

    const auto tokenUser = wil::get_token_information<TOKEN_USER>(GetCurrentProcessToken());
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
    THROW_IF_WIN32_BOOL_FALSE(CreateDirectoryW(m_fileSystemResources.SocketDirectory.c_str(), &attributes));
    m_fileSystemResources.DirectoryCreated = true;

    UniqueConfig config;
    THROW_IF_FAILED(WslOpenVmmCreateConfig(config.put()));
    THROW_IF_FAILED(WslOpenVmmConfigSetKernelPath(config.get(), Request.Boot.KernelPath.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetInitrdPath(config.get(), Request.Boot.InitrdPath.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetKernelCmdLine(config.get(), m_description.Boot.KernelCommandLine.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetMemoryMb(config.get(), m_description.Memory.SizeBytes / (1024 * 1024)));
    THROW_IF_FAILED(WslOpenVmmConfigSetProcessorCount(config.get(), Request.Processor.Count));
    THROW_IF_FAILED(WslOpenVmmConfigSetHvSocketPath(config.get(), m_fileSystemResources.VsockPath.c_str()));
    for (const auto& [tag, attachment] : m_description.NetworkAdapters)
    {
        const auto nicId =
            wsl::shared::string::GuidToString<wchar_t>(attachment.GuestInstanceId.value(), wsl::shared::string::GuidToStringFlags::None);
        const auto macAddress = wsl::shared::string::FormatMacAddress(attachment.EffectiveConfiguration.ClientMac.Bytes, L'-');
        THROW_IF_FAILED(WslOpenVmmConfigSetConsommeNic(config.get(), nicId.c_str(), macAddress.c_str()));
        m_networkAdapters.emplace(attachment.Id.Value, NetworkAdapter{attachment, nicId});
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
            THROW_IF_FAILED(WslOpenVmmConfigSetVirtioConsolePath(config.get(), std::get<VmVirtioConsole>(console.Device).NamedPipe.c_str()));
        }
    }

    m_job = helpers::CreateKillOnCloseJob();
    const auto commandLine =
        std::format(L"\"{}\" --rpc \"path={},transport=grpc\"", executable.native(), m_fileSystemResources.RpcSocketPath.native());
    SubProcess process{executable.c_str(), commandLine.c_str()};
    process.SetFlags(CREATE_NO_WINDOW);
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
    LOG_IF_WIN32_BOOL_FALSE(SetEvent(m_exitEvent.get()));
    auto lock = m_lock.lock_exclusive();
    CloseGuestListeners();
}

VmPlatformCapabilities OpenVmmVirtualMachineBackend::QueryCapabilities()
{
    VmPlatformCapabilities capabilities;
    capabilities.Backend = BackendKind::OpenVmm;
    // Report known OpenVMM support independently of which backend methods are wired through the C ABI.
    for (const auto operation :
         {VmOperation::Create,
          VmOperation::Start,
          VmOperation::Terminate,
          VmOperation::CreateGuestListener,
          VmOperation::AcceptGuestConnection,
          VmOperation::ConnectGuest,
          VmOperation::CloseGuestListener,
          VmOperation::AttachDisk,
          VmOperation::DetachDisk,
          VmOperation::CreateFileSystemDevice,
          VmOperation::AddFileSystemShare,
          VmOperation::RemoveFileSystemShare,
          VmOperation::RemoveDevice,
          VmOperation::UpdateNetworkAdapter,
          VmOperation::BindPort,
          VmOperation::UnbindPort})
    {
        capabilities.Operations.set(static_cast<size_t>(operation));
    }
    for (const auto feature :
         {VmFeature::LinuxDirectBoot,
          VmFeature::LinuxFirmwareBoot,
          VmFeature::MemoryOvercommit,
          VmFeature::SerialConsole,
          VmFeature::VirtioConsole,
          VmFeature::Vhd,
          VmFeature::Vhdx,
          VmFeature::VirtioFsFileBacked,
          VmFeature::SavedStateOnCrash,
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

wil::unique_handle OpenVmmVirtualMachineBackend::GetTerminationEvent() const
{
    return wil::unique_handle{wsl::windows::common::wslutil::DuplicateHandle(m_exitEvent.get())};
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
}

void OpenVmmVirtualMachineBackend::Terminate()
{
    const auto startTimeMs = GetTickCount64();
    WSL_LOG("OpenVmmTerminateVmBegin", TraceLoggingValue(m_description.Identity.VmId, "vmId"));
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    const auto teardownResult = WslOpenVmmVmTeardown(m_vm.get());
    WSL_LOG(
        "OpenVmmTeardownVm",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingHResult(teardownResult, "result"));
    THROW_IF_FAILED(teardownResult);
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
        THROW_HR(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
    }

    m_vm.reset();
    m_attachedDisks.clear();
    m_fileSystemShares.clear();
    m_fileSystemDevices.clear();
    m_portBindings.clear();
    m_networkAdapters.clear();
    CloseGuestListeners();
    WSL_LOG(
        "OpenVmmTerminateVmEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(GetTickCount64() - startTimeMs, "durationMs"));
}

VmGuestListener OpenVmmVirtualMachineBackend::CreateGuestListener(GuestServicePort Port)
{
    WSL_LOG(
        "OpenVmmCreateGuestListenerBegin",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Port.Value, "port"));
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    THROW_HR_IF(E_BOUNDS, m_nextListenerId == UINT64_MAX);
    for (const auto& entry : m_guestListeners)
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), entry.second->Listener.Port.Value == Port.Value);
    }

    auto listener = std::make_shared<GuestListener>();
    listener->Listener = {{m_description.Identity, m_nextListenerId}, Port};
    listener->Path = GetVsockListenerPath(m_fileSystemResources.VsockPath, Port);
    DeleteOwnedFile(listener->Path);

    listener->Socket.reset(::socket(AF_UNIX, SOCK_STREAM, 0));
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), !listener->Socket);

    const auto address = GetUnixSocketAddress(listener->Path);

    THROW_WIN32_IF(
        static_cast<DWORD>(WSAGetLastError()),
        bind(listener->Socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR);
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), listen(listener->Socket.get(), SOMAXCONN) == SOCKET_ERROR);

    const auto result = listener->Listener;
    const auto inserted = m_guestListeners.emplace(result.Id.Value, std::move(listener)).second;
    WI_ASSERT(inserted);
    ++m_nextListenerId;
    WSL_LOG(
        "OpenVmmCreateGuestListenerEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(result.Id.Value, "listenerId"),
        TraceLoggingValue(Port.Value, "port"));
    return result;
}

wil::unique_socket OpenVmmVirtualMachineBackend::AcceptGuestConnection(VmListenerId Listener)
{
    WSL_LOG(
        "OpenVmmAcceptGuestConnectionBegin",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Listener.Value, "listenerId"));
    ValidateResourceId(Listener, m_description.Identity);

    std::shared_ptr<GuestListener> listener;
    {
        auto lock = m_lock.lock_shared();
        const auto entry = m_guestListeners.find(Listener.Value);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), entry == m_guestListeners.end());
        listener = entry->second;
    }

    auto socket = wsl::windows::common::socket::CancellableAccept(listener->Socket.get(), INFINITE, listener->CancellationEvent.get());
    WSL_LOG(
        "OpenVmmAcceptGuestConnectionEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Listener.Value, "listenerId"),
        TraceLoggingValue(listener->Listener.Port.Value, "port"),
        TraceLoggingHResult(socket ? S_OK : E_ABORT, "result"));
    THROW_HR_IF(E_ABORT, !socket);
    return std::move(*socket);
}

wil::unique_socket OpenVmmVirtualMachineBackend::ConnectGuest(GuestServicePort Port)
{
    const auto startTimeMs = GetTickCount64();
    WSL_LOG(
        "OpenVmmConnectGuestBegin", TraceLoggingValue(m_description.Identity.VmId, "vmId"), TraceLoggingValue(Port.Value, "port"));
    {
        auto lock = m_lock.lock_shared();
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
        THROW_HR_IF(E_ABORT, m_exitEvent.is_signaled());
    }

    wil::unique_socket socket{::socket(AF_UNIX, SOCK_STREAM, 0)};
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), !socket);

    const auto address = GetUnixSocketAddress(m_fileSystemResources.VsockPath);

    THROW_WIN32_IF(
        static_cast<DWORD>(WSAGetLastError()), connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR);

    const auto request = std::format("CONNECT {}\n", Port.Value);
    wsl::windows::common::socket::Send(
        socket.get(), gsl::make_span(reinterpret_cast<const gsl::byte*>(request.data()), request.size()), m_exitEvent.get());

    std::array<char, 64> response{};
    size_t responseLength = 0;
    for (; responseLength < response.size() - 1; ++responseLength)
    {
        const auto bytesRead = wsl::windows::common::socket::Receive(
            socket.get(), gsl::make_span(reinterpret_cast<gsl::byte*>(&response[responseLength]), 1), m_exitEvent.get(), MSG_WAITALL, c_rpcTimeoutMs);
        THROW_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED), bytesRead == 0, "vsock bridge closed during CONNECT handshake");
        if (response[responseLength] == '\n')
        {
            ++responseLength;
            break;
        }
    }

    THROW_HR_IF_MSG(
        E_FAIL, responseLength == response.size() - 1 && response[responseLength - 1] != '\n', "vsock bridge response too long");
    const std::string_view responseView{response.data(), responseLength};
    THROW_HR_IF_MSG(E_FAIL, !responseView.starts_with("OK "), "vsock bridge CONNECT failed: %hs", response.data());
    WSL_LOG(
        "OpenVmmConnectGuestEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Port.Value, "port"),
        TraceLoggingValue(GetTickCount64() - startTimeMs, "durationMs"));
    return socket;
}

void OpenVmmVirtualMachineBackend::CloseGuestListener(VmListenerId Listener)
{
    WSL_LOG(
        "OpenVmmCloseGuestListener",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Listener.Value, "listenerId"));
    ValidateResourceId(Listener, m_description.Identity);
    auto lock = m_lock.lock_exclusive();
    const auto entry = m_guestListeners.find(Listener.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), entry == m_guestListeners.end());
    THROW_IF_WIN32_BOOL_FALSE(SetEvent(entry->second->CancellationEvent.get()));
    m_guestListeners.erase(entry);
}

VmDiskAttachment OpenVmmVirtualMachineBackend::AttachDisk(const VmDiskRequest& Request)
{
    const auto& source = GetVirtualDiskSource(Request);
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);

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
    const VmDiskAttachment attachment{{m_description.Identity, m_nextDiskId}, {0, lun}, Request.ReadOnly};
    const auto [disk, inserted] = m_attachedDisks.emplace(attachment.Id.Value, attachment);
    WI_ASSERT(inserted);
    auto rollback = wil::scope_exit([this, &disk] { m_attachedDisks.erase(disk); });
    const auto result = WslOpenVmmVmAttachScsiDisk(
        m_vm.get(), attachment.GuestAddress.Controller, attachment.GuestAddress.Lun, source.Path.c_str(), Request.ReadOnly);
    WSL_LOG(
        "OpenVmmAttachDiskEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(attachment.Id.Value, "diskId"),
        TraceLoggingValue(attachment.GuestAddress.Controller, "controller"),
        TraceLoggingValue(attachment.GuestAddress.Lun, "lun"),
        TraceLoggingValue(Request.ReadOnly, "readOnly"),
        TraceLoggingHResult(result, "result"));
    THROW_IF_FAILED(result);
    ++m_nextDiskId;
    rollback.release();
    return attachment;
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
    THROW_IF_FAILED(WslOpenVmmVmDetachScsiDisk(m_vm.get(), disk->second.GuestAddress.Controller, disk->second.GuestAddress.Lun));
    WSL_LOG(
        "OpenVmmDetachDiskEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Disk.Value, "diskId"),
        TraceLoggingValue(disk->second.GuestAddress.Controller, "controller"),
        TraceLoggingValue(disk->second.GuestAddress.Lun, "lun"));
    m_attachedDisks.erase(disk);
}

VmFileSystemDevice OpenVmmVirtualMachineBackend::CreateFileSystemDevice(const VmFileSystemDeviceRequest& Request)
{
    WSL_LOG(
        "OpenVmmCreateFileSystemDeviceBegin",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Request.Transport.Tag.c_str(), "tag"),
        TraceLoggingValue(static_cast<UINT32>(Request.Transport.Layout), "layout"));
    THROW_HR_IF_MSG(
        c_notSupported,
        Request.Transport.Layout != VmVirtioFsLayout::SingleShare,
        "OpenVMM currently supports only single-share virtio-fs devices");

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    THROW_HR_IF(E_BOUNDS, m_nextDeviceId == UINT64_MAX);
    for (const auto& entry : m_fileSystemDevices)
    {
        THROW_HR_IF(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), wsl::shared::string::IsEqual(entry.second.Transport.Tag, Request.Transport.Tag, false));
    }

    VmFileSystemDevice device{{m_description.Identity, m_nextDeviceId}, VmFileSystemDeviceState::Prepared};
    const auto inserted = m_fileSystemDevices.emplace(device.Id.Value, FileSystemDevice{device, Request.Transport, {}}).second;
    WI_ASSERT(inserted);
    ++m_nextDeviceId;
    WSL_LOG(
        "OpenVmmCreateFileSystemDeviceEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(device.Id.Value, "deviceId"),
        TraceLoggingValue(Request.Transport.Tag.c_str(), "tag"));
    return device;
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
    THROW_HR_IF_MSG(c_notSupported, !Request.Options.MountOptions.empty(), "OpenVMM does not support virtio-fs mount options");
    const auto hostPath = wsl::windows::common::filesystem::GetCanonicalPath(Request.HostPath);
    const auto attributes = GetFileAttributesW(hostPath.c_str());
    THROW_LAST_ERROR_IF(attributes == INVALID_FILE_ATTRIBUTES);
    THROW_HR_IF_MSG(
        E_INVALIDARG, WI_IsFlagClear(attributes, FILE_ATTRIBUTE_DIRECTORY), "The virtio-fs host path must be a directory");

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    const auto device = m_fileSystemDevices.find(Device.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), device == m_fileSystemDevices.end());
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), device->second.Share.has_value());
    THROW_HR_IF(E_BOUNDS, m_nextShareId == UINT64_MAX);

    VmFileSystemShare share{{m_description.Identity, m_nextShareId}, Device, {device->second.Transport.Tag, {}}, hostPath, Request.ReadOnly};
    const auto [entry, inserted] = m_fileSystemShares.emplace(share.Id.Value, FileSystemShare{share});
    WI_ASSERT(inserted);
    device->second.Share = share.Id.Value;
    device->second.Device.State = VmFileSystemDeviceState::Serving;
    auto rollback = wil::scope_exit([this, &device, &entry] {
        device->second.Share.reset();
        device->second.Device.State = VmFileSystemDeviceState::Prepared;
        m_fileSystemShares.erase(entry);
    });
    const auto result = WslOpenVmmVmAddShare(m_vm.get(), share.GuestAddress.Tag.c_str(), hostPath.c_str(), Request.ReadOnly);
    WSL_LOG(
        "OpenVmmAddFileSystemShareEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(share.Id.Value, "shareId"),
        TraceLoggingValue(share.GuestAddress.Tag.c_str(), "tag"),
        TraceLoggingHResult(result, "result"));
    THROW_IF_FAILED(result);
    ++m_nextShareId;
    rollback.release();
    return share;
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
    const auto device = m_fileSystemDevices.find(share->second.Share.Device.Value);
    THROW_HR_IF(E_UNEXPECTED, device == m_fileSystemDevices.end() || device->second.Share != Share.Value);

    const auto result = WslOpenVmmVmRemoveShare(m_vm.get(), share->second.Share.GuestAddress.Tag.c_str());
    WSL_LOG(
        "OpenVmmRemoveFileSystemShareEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Share.Value, "shareId"),
        TraceLoggingValue(device->second.Device.Id.Value, "deviceId"),
        TraceLoggingValue(share->second.Share.GuestAddress.Tag.c_str(), "tag"),
        TraceLoggingHResult(result, "result"));
    THROW_IF_FAILED(result);
    device->second.Share.reset();
    device->second.Device.State = VmFileSystemDeviceState::Prepared;
    m_fileSystemShares.erase(share);
}

VmNetworkAttachment OpenVmmVirtualMachineBackend::AddNetworkAdapter(const VmNetworkAdapterRequest& Request)
{
    ExecutionContext context(Context::ConfigureNetworking);
    THROW_HR_MSG(c_notSupported, "OpenVMM network adapter '%ls' must be configured at VM creation time", Request.Tag.c_str());
}

VmPortBinding OpenVmmVirtualMachineBackend::BindPort(VmDeviceId Device, const VmPortBindingRequest& Request)
{
    ExecutionContext context(Context::ConfigureNetworking);
    WSL_LOG(
        "OpenVmmBindPortBegin",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(static_cast<UINT32>(Request.Protocol), "protocol"),
        TraceLoggingValue(Request.Listen.Port, "hostPort"),
        TraceLoggingValue(Request.GuestPort, "guestPort"));
    ValidateResourceId(Device, m_description.Identity);
    THROW_HR_IF_MSG(
        c_notSupported, Request.Listen.Port == 0, "OpenVMM cannot report the allocated port for a dynamic host port binding");
    THROW_HR_IF(E_INVALIDARG, Request.GuestPort == 0);
    const auto hostAddress = FormatIpAddress(Request.Listen.Address);
    bool tcp = false;
    switch (Request.Protocol)
    {
    case VmTransportProtocol::Tcp:
        tcp = true;
        break;
    case VmTransportProtocol::Udp:
        break;
    default:
        THROW_HR(E_INVALIDARG);
    }

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    const auto adapter = m_networkAdapters.find(Device.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), adapter == m_networkAdapters.end());
    THROW_HR_IF(E_BOUNDS, m_nextPortBindingId == UINT64_MAX);
    for (const auto& entry : m_portBindings)
    {
        THROW_HR_IF(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
            entry.second.Binding.Protocol == Request.Protocol && entry.second.Binding.EffectiveListen.Port == Request.Listen.Port &&
                wsl::shared::string::IsEqual(entry.second.HostAddress, hostAddress, false));
    }

    VmPortBinding binding{{m_description.Identity, m_nextPortBindingId}, Device, Request.Protocol, Request.Listen, Request.GuestPort};
    const auto [entry, inserted] = m_portBindings.emplace(binding.Id.Value, PortBinding{binding, adapter->second.NicId, hostAddress});
    WI_ASSERT(inserted);
    auto rollback = wil::scope_exit([this, &entry] { m_portBindings.erase(entry); });
    const auto result =
        WslOpenVmmVmBindPort(m_vm.get(), adapter->second.NicId.c_str(), Request.Listen.Port, Request.GuestPort, tcp, hostAddress.c_str());
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
        binding->second.NicId.c_str(),
        binding->second.Binding.EffectiveListen.Port,
        binding->second.Binding.GuestPort,
        binding->second.Binding.Protocol == VmTransportProtocol::Tcp,
        binding->second.HostAddress.c_str());
    WSL_LOG(
        "OpenVmmUnbindPortEnd",
        TraceLoggingValue(m_description.Identity.VmId, "vmId"),
        TraceLoggingValue(Binding.Value, "bindingId"),
        TraceLoggingValue(binding->second.Binding.Device.Value, "deviceId"),
        TraceLoggingHResult(result, "result"));
    THROW_IF_FAILED(result);
    m_portBindings.erase(binding);
}