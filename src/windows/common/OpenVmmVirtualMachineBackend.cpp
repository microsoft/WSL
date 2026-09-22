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

namespace {

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

} // namespace

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
    description.Memory.SizeBytes = Request.Memory.SizeBytes;
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
            if (placement.Address.Lun < c_maximumDisks)
            {
                allocated.set(placement.Address.Lun);
            }
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
            THROW_HR_IF(E_BOUNDS, lun == c_maximumDisks);
            allocated.set(lun);
        }
        description.BootDisks.at(disk.Key) = {{description.Identity, nextId++}, {0, lun}, disk.Disk.ReadOnly};
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
    if (m_processWait)
    {
        m_processWait.reset();
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
    if (m_directoryCreated)
    {
        LOG_IF_FAILED(wil::RemoveDirectoryRecursiveNoThrow(m_socketDirectory.c_str()));
    }
}

std::unique_ptr<OpenVmmVirtualMachineBackend> OpenVmmVirtualMachineBackend::Create(const VmCreateRequest& Request)
{
    auto description = wsl::windows::common::vm::openvmm::ValidateCreateRequest(Request);
    auto backend = std::unique_ptr<OpenVmmVirtualMachineBackend>{new OpenVmmVirtualMachineBackend{}};
    backend->m_description = std::move(description);
    backend->Initialize(Request);
    return backend;
}

void OpenVmmVirtualMachineBackend::Initialize(const VmCreateRequest& Request)
{
    using namespace wsl::windows::common;
    const auto executable = wslutil::GetBasePath() / L"openvmm.exe";

    auto id = wsl::shared::string::GuidToString<wchar_t>(Request.VmId, wsl::shared::string::GuidToStringFlags::None);
    std::erase(id, L'-');
    // An exclusive directory creation prevents shortened path IDs from aliasing another VM.
    m_socketDirectory = filesystem::GetTempFolderPath(GetCurrentProcessToken()) / (L"ov-" + id.substr(0, 16));
    m_rpcSocketPath = m_socketDirectory / L"r";
    m_vsockPath = m_socketDirectory / L"v";
    constexpr size_t c_guidStringLength = 38;
    constexpr size_t c_guestSocketSuffixLength = 1 + c_guidStringLength;
    const auto vsockPath = wsl::shared::string::WideToMultiByte(m_vsockPath.native());
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
    THROW_IF_WIN32_BOOL_FALSE(CreateDirectoryW(m_socketDirectory.c_str(), &attributes));
    m_directoryCreated = true;

    UniqueConfig config;
    THROW_IF_FAILED(WslOpenVmmCreateConfig(config.put()));
    THROW_IF_FAILED(WslOpenVmmConfigSetKernelPath(config.get(), Request.Boot.KernelPath.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetInitrdPath(config.get(), Request.Boot.InitrdPath.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetKernelCmdLine(config.get(), m_description.Boot.KernelCommandLine.c_str()));
    THROW_IF_FAILED(WslOpenVmmConfigSetMemoryMb(config.get(), m_description.Memory.SizeBytes / (1024 * 1024)));
    THROW_IF_FAILED(WslOpenVmmConfigSetProcessorCount(config.get(), Request.Processor.Count));
    THROW_IF_FAILED(WslOpenVmmConfigSetHvSocketPath(config.get(), m_vsockPath.c_str()));
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
        std::format(L"\"{}\" --rpc \"path={},transport=grpc\"", executable.native(), m_rpcSocketPath.native());
    SubProcess process{executable.c_str(), commandLine.c_str()};
    process.SetFlags(CREATE_NO_WINDOW);
    process.SetJobObject(m_job.get());
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    wil::unique_hfile input;
    input.reset(CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    THROW_LAST_ERROR_IF(!input);
    wil::unique_hfile output{CreateFileW(
        L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    THROW_LAST_ERROR_IF(!output);
    auto [logPipeRead, logPipeWrite] = wslutil::OpenAnonymousPipe(0, true, false);
    THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(logPipeWrite.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
    process.SetStdHandles(input.get(), output.get(), logPipeWrite.get());
    m_process = process.Start();
    logPipeWrite.reset();
    m_processLogThread = std::thread(&OpenVmmVirtualMachineBackend::ReadProcessLog, this, std::move(logPipeRead));
    m_processWait.reset(CreateThreadpoolWait(OnProcessExit, this, nullptr));
    THROW_LAST_ERROR_IF(!m_processWait);
    SetThreadpoolWait(m_processWait.get(), m_process.get(), nullptr);
    THROW_IF_FAILED_MSG(
        WslOpenVmmCreateVm(config.addressof(), m_rpcSocketPath.c_str(), c_rpcTimeoutMs, m_vm.put()),
        "Failed to create OpenVMM VM");
    const auto result = WaitForSingleObject(m_process.get(), 0);
    THROW_LAST_ERROR_IF(result == WAIT_FAILED);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED), result == WAIT_OBJECT_0);
}

void OpenVmmVirtualMachineBackend::ReadProcessLog(wil::unique_hfile Pipe) noexcept
try
{
    wsl::windows::common::io::MultiHandleWait io;
    io.AddHandle(std::make_unique<wsl::windows::common::io::ReadHandle>(
        std::move(Pipe),
        [this](const gsl::span<char>& Buffer) {
            if (!Buffer.empty())
            {
                const std::string entry{Buffer.begin(), Buffer.end()};
                WSL_LOG(
                    "OpenVmmLog",
                    TraceLoggingGuid(m_description.Identity.VmId, "VmId"),
                    TraceLoggingValue(entry.c_str(), "Content"));
            }
        }));
    io.AddHandle(std::make_unique<wsl::windows::common::io::EventHandle>(m_process.get()));
    io.Run(std::nullopt);
}
CATCH_LOG()

void CALLBACK OpenVmmVirtualMachineBackend::OnProcessExit(PTP_CALLBACK_INSTANCE, void* Context, PTP_WAIT, TP_WAIT_RESULT) noexcept
{
    auto& backend = *static_cast<OpenVmmVirtualMachineBackend*>(Context);
    LOG_IF_WIN32_BOOL_FALSE(SetEvent(backend.m_exitEvent.get()));
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
          VmOperation::AddNetworkAdapter,
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

wil::unique_handle OpenVmmVirtualMachineBackend::GetTerminationEvent() const
{
    wil::unique_handle event;
    THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(
        GetCurrentProcess(), m_exitEvent.get(), GetCurrentProcess(), event.put(), 0, FALSE, DUPLICATE_SAME_ACCESS));
    return event;
}

void OpenVmmVirtualMachineBackend::Start()
{
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    THROW_IF_FAILED(WslOpenVmmVmResume(m_vm.get()));
}

void OpenVmmVirtualMachineBackend::Terminate()
{
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    THROW_IF_FAILED(WslOpenVmmVmTeardown(m_vm.get()));
    const auto quitResult = WslOpenVmmVmQuit(m_vm.get());
    const auto waitResult = WaitForSingleObject(m_process.get(), c_rpcTimeoutMs);
    THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);
    if (waitResult != WAIT_OBJECT_0)
    {
        THROW_IF_FAILED(quitResult);
        THROW_HR(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
    }

    m_vm.reset();
    m_attachedDisks.clear();
}

void OpenVmmVirtualMachineBackend::CancelPendingOperations() noexcept
{
    LOG_HR(E_NOTIMPL);
}

VmGuestListener OpenVmmVirtualMachineBackend::CreateGuestListener(GuestServicePort)
{
    THROW_HR(E_NOTIMPL);
}

wil::unique_socket OpenVmmVirtualMachineBackend::AcceptGuestConnection(VmListenerId)
{
    THROW_HR(E_NOTIMPL);
}

wil::unique_socket OpenVmmVirtualMachineBackend::ConnectGuest(GuestServicePort)
{
    THROW_HR(E_NOTIMPL);
}

void OpenVmmVirtualMachineBackend::CloseGuestListener(VmListenerId)
{
    THROW_HR(E_NOTIMPL);
}

VmDiskAttachment OpenVmmVirtualMachineBackend::AttachDisk(const VmDiskRequest& Request)
{
    const auto& source = GetVirtualDiskSource(Request);
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);

    const auto lunInUse = [&](std::uint32_t Lun) {
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
        lun = Request.Placement->Address.Lun;
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
        {m_description.Identity, m_nextDiskId}, {0, lun}, Request.ReadOnly};
    const auto [disk, inserted] = m_attachedDisks.emplace(attachment.Id.Value, attachment);
    WI_ASSERT(inserted);
    auto rollback = wil::scope_exit([&] { m_attachedDisks.erase(disk); });
    THROW_IF_FAILED(WslOpenVmmVmAttachScsiDisk(
        m_vm.get(), attachment.GuestAddress.Controller, attachment.GuestAddress.Lun, source.Path.c_str(), Request.ReadOnly));
    ++m_nextDiskId;
    rollback.release();
    return attachment;
}

void OpenVmmVirtualMachineBackend::DetachDisk(VmDiskId Disk)
{
    THROW_HR_IF(E_INVALIDARG, Disk.Value == 0 || !IsEqualGUID(Disk.Owner.VmId, m_description.Identity.VmId));
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vm);
    const auto disk = m_attachedDisks.find(Disk.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), disk == m_attachedDisks.end());
    THROW_IF_FAILED(WslOpenVmmVmDetachScsiDisk(
        m_vm.get(), disk->second.GuestAddress.Controller, disk->second.GuestAddress.Lun));
    m_attachedDisks.erase(disk);
}

VmFileSystemDevice OpenVmmVirtualMachineBackend::CreateFileSystemDevice(const VmFileSystemDeviceRequest&)
{
    THROW_HR(E_NOTIMPL);
}

VmFileSystemShare OpenVmmVirtualMachineBackend::AddFileSystemShare(VmDeviceId, const VmFileSystemShareRequest&)
{
    THROW_HR(E_NOTIMPL);
}

void OpenVmmVirtualMachineBackend::RemoveFileSystemShare(VmShareId)
{
    THROW_HR(E_NOTIMPL);
}

VmNetworkAttachment OpenVmmVirtualMachineBackend::AddNetworkAdapter(const VmNetworkAdapterRequest&)
{
    THROW_HR(E_NOTIMPL);
}

VmPortBinding OpenVmmVirtualMachineBackend::BindPort(VmDeviceId, const VmPortBindingRequest&)
{
    THROW_HR(E_NOTIMPL);
}

void OpenVmmVirtualMachineBackend::UnbindPort(VmPortBindingId)
{
    THROW_HR(E_NOTIMPL);
}