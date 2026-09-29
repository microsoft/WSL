/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    HcsVirtualMachineBackend.cpp

Abstract:

    Implementation of IVirtualMachineBackend - represents a single HCS-based VM instance.

--*/

#include "precomp.h"
#include "ExecutionContext.h"
#include "HcsVirtualMachineBackend.h"
#include "hvsocket.hpp"

using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;

namespace validation = wsl::windows::common::vm::validation;

namespace {

namespace schema = wsl::windows::common::hcs;

constexpr UINT64 c_mib = 1024 * 1024;
constexpr UINT32 c_maximumDisks = 254;
constexpr HRESULT c_notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

template <typename... Visitors>
struct Overloaded : Visitors...
{
    using Visitors::operator()...;
};

template <typename... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

// Virtio-fs and Plan9 virtio devices are addressed by tag; Plan9 socket devices have none.
std::wstring GetFileSystemDeviceTag(const VmFileSystemDeviceTransport& Transport)
{
    return std::visit(
        Overloaded{
            [](const VmVirtioFsDevice& transport) { return transport.Tag; },
            [](const VmPlan9VirtioDevice& transport) { return transport.Tag; },
            [](const VmPlan9SocketDevice&) { return std::wstring{}; }},
        Transport);
}

// Plan9 socket devices are addressed by the port their server listens on.
std::optional<std::uint32_t> GetPlan9SocketPort(const VmFileSystemDeviceTransport& Transport)
{
    return std::visit(
        Overloaded{
            [](const VmPlan9SocketDevice& transport) { return std::optional<std::uint32_t>{transport.Port.Value}; },
            [](const VmVirtioFsDevice&) { return std::optional<std::uint32_t>{}; },
            [](const VmPlan9VirtioDevice&) { return std::optional<std::uint32_t>{}; }},
        Transport);
}

// A share whose name the caller left unspecified is named after a new GUID, which is short enough to
// also be usable as a virtio-fs tag.
std::wstring GenerateShareName()
{
    GUID name{};
    THROW_IF_FAILED(CoCreateGuid(&name));

    return wsl::shared::string::GuidToString<wchar_t>(name, wsl::shared::string::GuidToStringFlags::None);
}

VmEffectiveProcessor ConfigureProcessor(const VmProcessorRequest& Request, schema::Processor& Settings)
{
    VmEffectiveProcessor processor{};
    processor.Count = Request.Count;
    Settings.Count = processor.Count;

    const bool nestedVirtualizationSupported =
        Request.NestedVirtualization == VmFeatureRequest::Disabled ? false : schema::IsNestedVirtualizationSupported();
    processor.NestedVirtualization =
        validation::ValidateFeature(Request.NestedVirtualization, L"nested virtualization", nestedVirtualizationSupported);
    Settings.ExposeVirtualizationExtensions = processor.NestedVirtualization;

    const auto [perfmonPmuSupported, perfmonLbrSupported] =
        Request.PerfmonPmu == VmFeatureRequest::Disabled && Request.PerfmonLbr == VmFeatureRequest::Disabled
            ? std::pair<bool, bool>{}
            : schema::GetPerfmonCapabilities();
    processor.PerfmonPmu = validation::ValidateFeature(Request.PerfmonPmu, L"PMU", perfmonPmuSupported);
    processor.PerfmonLbr = validation::ValidateFeature(Request.PerfmonLbr, L"LBR", perfmonLbrSupported);
    Settings.EnablePerfmonPmu = processor.PerfmonPmu;
    Settings.EnablePerfmonLbr = processor.PerfmonLbr;

    return processor;
}

VmEffectiveMemory ConfigureMemory(const VmMemoryRequest& Request, schema::Memory& Settings)
{
    VmEffectiveMemory memory{};
    memory.SizeBytes = (Request.SizeBytes / c_mib) * c_mib;
    memory.AllowOvercommit = validation::ValidateFeature(Request.AllowOvercommit, L"memory overcommit", true);
    memory.DeferredCommit = validation::ValidateFeature(Request.DeferredCommit, L"deferred memory commit", true);
    memory.ColdDiscard = validation::ValidateFeature(Request.ColdDiscard, L"cold discard", true);

    Settings.SizeInMB = memory.SizeBytes / c_mib;
    Settings.AllowOvercommit = memory.AllowOvercommit;
    Settings.EnableDeferredCommit = memory.DeferredCommit;
    Settings.EnableColdDiscardHint = memory.ColdDiscard;
    return memory;
}

VmEffectiveBoot ConfigureBoot(const VmLinuxBootRequest& Request, schema::Chipset& Settings)
{
    VmEffectiveBoot boot{};
    boot.Method = Request.Method;
    switch (boot.Method)
    {
    case VmBootMethod::Automatic:
        if constexpr (wsl::shared::Arm64)
        {
            boot.Method = VmBootMethod::Uefi;
        }
        else
        {
            boot.Method = VmBootMethod::LinuxDirect;
        }
        break;
    case VmBootMethod::LinuxDirect:
    case VmBootMethod::Uefi:
        break;
    default:
        break;
    }

    boot.KernelCommandLine = Request.KernelCommandLine;
    Settings.UseUtc = true;
    switch (boot.Method)
    {
    case VmBootMethod::LinuxDirect:
        THROW_HR_IF_MSG(c_notSupported, wsl::shared::Arm64, "HCS Linux direct boot is currently supported only on x64");
        Settings.LinuxKernelDirect =
            schema::LinuxKernelDirect{Request.KernelPath.native(), Request.InitrdPath.native(), boot.KernelCommandLine};
        break;
    case VmBootMethod::Uefi:
    {
        THROW_HR_IF_MSG(c_notSupported, !Request.InitrdPath.empty(), "HCS UEFI boot does not support an initrd");
        const auto kernelName = Request.KernelPath.filename().native();
        Settings.Uefi = schema::Uefi{
            {schema::UefiBootDevice::VmbFs, Request.KernelPath.parent_path().native(), L"\\" + kernelName, boot.KernelCommandLine}};
        break;
    }
    default:
        break;
    }

    return boot;
}

} // namespace

HcsVirtualMachineBackend::VmConfiguration HcsVirtualMachineBackend::BuildConfiguration(const VmCreateRequest& Request)
{
    auto signalEarlyTermination = wil::scope_exit([&] { m_terminatingEvent.SetEvent(); });

    THROW_HR_IF(E_INVALIDARG, IsEqualGUID(Request.Identity.VmId, GUID_NULL));
    m_restrictedToken = wsl::windows::common::security::CreateRestrictedToken(Request.Identity.UserToken.get());

    VmConfiguration configuration{};
    configuration.Settings.Owner = Request.Owner;
    configuration.Settings.ShouldTerminateOnLastHandleClosed = true;
    configuration.Settings.SchemaVersion.Major = 2;
    configuration.Settings.SchemaVersion.Minor = wsl::windows::common::helpers::IsWindows11OrAbove() ? 7 : 3;
    auto& description = configuration.Description;
    description.Identity = Request.Identity;
    description.Backend = BackendKind::Hcs;
    description.Processor = ConfigureProcessor(Request.Processor, configuration.Settings.VirtualMachine.ComputeTopology.Processor);
    description.Memory = ConfigureMemory(Request.Memory, configuration.Settings.VirtualMachine.ComputeTopology.Memory);
    description.Boot = ConfigureBoot(Request.Boot, configuration.Settings.VirtualMachine.Chipset);
    configuration.Settings.VirtualMachine.Devices.Scsi["0"] = {};

    // Permit the VM identity and SYSTEM to bind and connect Hyper-V sockets. Plan 9 socket
    // transports initialize their listener while impersonating the VM identity.
    const auto tokenUser = wil::get_token_information<TOKEN_USER>(Request.Identity.UserToken.get());
    wil::unique_hlocal_string userSid;
    THROW_LAST_ERROR_IF(!ConvertSidToStringSidW(tokenUser->User.Sid, &userSid));
    const auto securityDescriptor = std::format(L"D:P(A;;FA;;;SY)(A;;FA;;;{})", userSid.get());
    auto& hvSocket = configuration.Settings.VirtualMachine.Devices.HvSocket.HvSocketConfig;
    hvSocket.DefaultBindSecurityDescriptor = securityDescriptor;
    hvSocket.DefaultConnectSecurityDescriptor = securityDescriptor;

    signalEarlyTermination.release();
    return configuration;
}

HcsVirtualMachineBackend::HcsVirtualMachineBackend() = default;

HcsVirtualMachineBackend::~HcsVirtualMachineBackend() noexcept
{
    schema::unique_hcs_system system;
    std::map<std::uint64_t, AttachedDisk> attachedDisks;
    {
        auto lock = m_lock.lock_exclusive();
        CloseFileSystemDevicesLocked();
        system = std::move(m_system);
        attachedDisks = std::move(m_attachedDisks);
        CloseGuestListenersLocked(m_configuration.Description.Identity);
    }

    system.reset();
    CleanupAttachedDisks(std::move(attachedDisks));

    auto exitDetailsLock = m_exitDetailsLock.lock_shared();
    WSL_LOG(
        "HcsVirtualMachineBackendDestroyed",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(m_exitEvent.is_signaled(), "exitEventSignaled"),
        TraceLoggingValue(m_exitDetails.c_str(), "exitDetails"));
}

std::unique_ptr<HcsVirtualMachineBackend> HcsVirtualMachineBackend::Create(const VmCreateRequest& Request)
{
    ExecutionContext context(Context::CreateVm);
    auto newInstance = std::unique_ptr<HcsVirtualMachineBackend>{new HcsVirtualMachineBackend{}};
    try
    {
        newInstance->m_configuration.Description.Identity = Request.Identity;
        newInstance->m_configuration.Description.Backend = BackendKind::Hcs;
        if (Request.CrashCapture && !Request.CrashCapture->Path.empty())
        {
            newInstance->m_crashCapture = Request.CrashCapture;
        }

        const auto startTimeMs = GetTickCount64();
        WSL_LOG_TELEMETRY("CreateVmBegin", PDT_ProductAndServicePerformance, TraceLoggingValue(Request.Identity.VmId, "vmId"));

        newInstance->Initialize(Request);

        WSL_LOG_TELEMETRY(
            "CreateVmEnd",
            PDT_ProductAndServicePerformance,
            TraceLoggingValue(Request.Identity.VmId, "vmId"),
            TraceLoggingValue(GetTickCount64() - startTimeMs, "timeToCreateVmMs"));
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();

        if (hr == HRESULT_FROM_WIN32(WSAENOTCONN) || hr == HRESULT_FROM_WIN32(WSAECONNRESET) || hr == HRESULT_FROM_WIN32(WSAETIMEDOUT))
        {
            // A kernel panic can cause an hvsocket error. Wait for an HCS notification to provide a better error for the user.
            if (newInstance->m_vmCrashEvent.wait(1000))
            {
                if (newInstance->m_vmCrashLogFile.has_value())
                {
                    THROW_HR_WITH_USER_ERROR(
                        WSL_E_VM_CRASHED,
                        wsl::shared::Localization::MessageWSL2Crashed() + L"\r\n" +
                            wsl::shared::Localization::MessageWSL2CrashedStackTrace(newInstance->m_vmCrashLogFile.value()));
                }
                else
                {
                    THROW_HR_WITH_USER_ERROR(WSL_E_VM_CRASHED, wsl::shared::Localization::MessageWSL2Crashed());
                }
            }
        }

        WSL_LOG_TELEMETRY(
            "FailedToStartVm",
            PDT_ProductAndServicePerformance,
            TraceLoggingValue(Request.Identity.VmId, "vmId"),
            TraceLoggingValue(hr, "error"));
        throw;
    }
    return newInstance;
}

void HcsVirtualMachineBackend::Initialize(const VmCreateRequest& Request)
{
    auto configuration = BuildConfiguration(Request);
    const auto id = wsl::shared::string::GuidToString<wchar_t>(Request.Identity.VmId, wsl::shared::string::GuidToStringFlags::None);
    const auto settings = wsl::shared::ToJsonW(configuration.Settings);
    m_configuration = std::move(configuration);
    m_vmIdString = id;
    auto lock = m_lock.lock_exclusive();
    m_system = schema::CreateComputeSystem(id.c_str(), settings.c_str());
    m_runtimeId = wsl::windows::common::hcs::GetRuntimeId(m_system.get());
    m_guestDeviceManager = std::make_shared<GuestDeviceManager>(id, m_runtimeId, true);
    schema::RegisterCallback(m_system.get(), OnSystemEvent, this);
}

VmPlatformCapabilities HcsVirtualMachineBackend::GetCapabilities() const
{
    return QueryCapabilities();
}

VmPlatformCapabilities HcsVirtualMachineBackend::QueryCapabilities()
{
    VmPlatformCapabilities capabilities{};
    capabilities.Backend = BackendKind::Hcs;
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
          VmOperation::RemoveFileSystemShare})
    {
        capabilities.Operations.set(static_cast<size_t>(operation));
    }

    for (const auto feature :
         {VmFeature::LinuxFirmwareBoot, VmFeature::MemoryOvercommit, VmFeature::DeferredMemoryCommit, VmFeature::ColdDiscard, VmFeature::Vhd, VmFeature::Vhdx, VmFeature::PhysicalDisk})
    {
        capabilities.Features.set(static_cast<size_t>(feature));
    }

    if constexpr (!wsl::shared::Arm64)
    {
        capabilities.Features.set(static_cast<size_t>(VmFeature::LinuxDirectBoot));
    }

    if (schema::IsNestedVirtualizationSupported())
    {
        capabilities.Features.set(static_cast<size_t>(VmFeature::NestedVirtualization));
    }

    const auto [perfmonPmuSupported, perfmonLbrSupported] = schema::GetPerfmonCapabilities();
    capabilities.Features.set(static_cast<size_t>(VmFeature::PerfmonPmu), perfmonPmuSupported);
    capabilities.Features.set(static_cast<size_t>(VmFeature::PerfmonLbr), perfmonLbrSupported);
    return capabilities;
}

VmDescription HcsVirtualMachineBackend::GetDescription() const
{
    return m_configuration.Description;
}

wil::unique_handle HcsVirtualMachineBackend::GetTerminationEvent() const
{
    wil::unique_handle event;
    THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(
        GetCurrentProcess(), m_terminatingEvent.get(), GetCurrentProcess(), event.put(), 0, FALSE, DUPLICATE_SAME_ACCESS));
    return event;
}

void HcsVirtualMachineBackend::Start()
{
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system);
    const auto settings = wsl::shared::ToJsonW(m_configuration.Settings);
    schema::StartComputeSystem(m_system.get(), settings.c_str());
}

void HcsVirtualMachineBackend::Terminate()
{
    schema::unique_hcs_system system;
    std::map<std::uint64_t, AttachedDisk> attachedDisks;
    {
        auto lock = m_lock.lock_exclusive();
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system);
        CloseFileSystemDevicesLocked();
        system = std::move(m_system);
        attachedDisks = std::move(m_attachedDisks);
        CloseGuestListenersLocked(m_configuration.Description.Identity);
    }

    auto cleanup = wil::scope_exit([&] {
        system.reset();
        CleanupAttachedDisks(std::move(attachedDisks));
        m_terminatingEvent.SetEvent();
        NotifyTerminated(m_configuration.Description.Identity);
    });

    schema::TerminateComputeSystem(system.get());
}

std::shared_ptr<VmGuestListenerState> HcsVirtualMachineBackend::ConfigureGuestListener(const VmGuestListener& Listener)
{
    auto state = std::make_shared<VmGuestListenerState>();
    state->Listener = Listener;
    state->Socket = wsl::windows::common::hvsocket::Listen(Listener.Id.Owner.VmId, Listener.Port.Value);
    return state;
}

VmGuestListener HcsVirtualMachineBackend::CreateGuestListener(GuestServicePort Port)
{
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system);
    return RegisterGuestListenerLocked(m_configuration.Description.Identity, Port);
}

wil::unique_socket HcsVirtualMachineBackend::AcceptGuestConnection(VmListenerId Listener)
{
    return AcceptGuestListenerConnection(Listener, m_configuration.Description.Identity);
}

wil::unique_socket HcsVirtualMachineBackend::ConnectGuest(GuestServicePort Port)
{
    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system);
    return wsl::windows::common::hvsocket::Connect(m_configuration.Description.Identity.VmId, Port.Value);
}

void HcsVirtualMachineBackend::CloseGuestListener(VmListenerId Listener)
{
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system);
    RemoveGuestListenerLocked(Listener, m_configuration.Description.Identity);
}

std::uint32_t HcsVirtualMachineBackend::ReserveLunLocked(const std::optional<VmScsiPlacement>& Placement) const
{
    const auto lunInUse = [this](std::uint32_t Lun) {
        for (const auto& entry : m_attachedDisks)
        {
            if (entry.second.Attachment.GuestAddress.Lun == Lun)
            {
                return true;
            }
        }
        return false;
    };

    std::uint32_t lun = 0;
    if (Placement)
    {
        const auto& address = Placement->Address;
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

    return lun;
}

std::map<std::uint64_t, HcsVirtualMachineBackend::AttachedDisk>::iterator HcsVirtualMachineBackend::FindAttachedDiskLocked(
    bool PassThrough, const std::wstring& Path)
{
    return std::find_if(m_attachedDisks.begin(), m_attachedDisks.end(), [&](const auto& entry) {
        return entry.second.PassThrough == PassThrough && wsl::windows::common::string::IsPathComponentEqual(entry.second.Path, Path);
    });
}

void HcsVirtualMachineBackend::CleanupAttachedDisks(std::map<std::uint64_t, AttachedDisk>&& Disks) noexcept
{
    for (const auto& entry : Disks)
    {
        const auto& disk = entry.second;
        if (WI_IsFlagSet(disk.Flags, wsl::windows::common::disk::DiskStateFlags::AccessGranted))
        {
            try
            {
                schema::RevokeVmAccess(m_vmIdString.c_str(), disk.Path.c_str());
            }
            CATCH_LOG()
        }

        if (WI_IsFlagSet(disk.Flags, wsl::windows::common::disk::DiskStateFlags::Online))
        {
            try
            {
                wsl::windows::common::disk::BringOnline(disk.Path.c_str(), static_cast<size_t>(disk.DeviceTimeout.count()));
            }
            CATCH_LOG()
        }
    }
}

VmDiskAttachment HcsVirtualMachineBackend::AttachDisk(const VmDiskRequest& Request)
{
    ExecutionContext context(Context::MountDisk);

    const auto& path = validation::ValidateDiskSource(Request);
    const bool passThrough = std::holds_alternative<VmPhysicalDiskSource>(Request.Source);
    const auto timeoutMs = static_cast<size_t>(Request.DeviceTimeout.count());

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system);
    THROW_HR_IF(E_BOUNDS, m_nextDiskId == UINT64_MAX);

    // Set scope exit variables to perform cleanup if attaching the disk fails.
    wsl::windows::common::disk::DiskStateFlags diskFlags{};
    wil::unique_hfile backingFile;
    std::optional<VmDiskAttachment> existingAttachment;
    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        if (WI_IsFlagSet(diskFlags, wsl::windows::common::disk::DiskStateFlags::AccessGranted))
        {
            schema::RevokeVmAccess(m_vmIdString.c_str(), path.c_str());
        }

        if (WI_IsFlagSet(diskFlags, wsl::windows::common::disk::DiskStateFlags::Online))
        {
            wsl::windows::common::disk::BringOnline(path.c_str(), timeoutMs);
        }
    });

    // Failures are reported with the same user-facing message as WslCoreVm::AttachDiskLockHeld.
    const auto wrapAttachFailure = [&](auto&& Routine) {
        try
        {
            Routine();
        }
        catch (...)
        {
            const auto result = wil::ResultFromCaughtException();
            THROW_HR_WITH_USER_ERROR(
                result, wsl::shared::Localization::MessageFailedToAttachDisk(path.c_str(), wsl::windows::common::wslutil::GetSystemErrorString(result)));
        }
    };

    // Check if the disk is already attached.
    //
    // N.B. This runs before reserving a LUN so that a request targeting the LUN of the disk being
    //      reattached is reported as a duplicate, and so that a stale attachment frees its LUN.
    wrapAttachFailure([&] {
        const auto found = FindAttachedDiskLocked(passThrough, path);
        if (found == m_attachedDisks.end())
        {
            return;
        }

        if (passThrough)
        {
            THROW_HR_WITH_USER_ERROR(WSL_E_DISK_ALREADY_ATTACHED, wsl::shared::Localization::MessageDiskAlreadyAttached(path.c_str()));
        }

        // Prevent user from launching a distro vhd after manually mounting it; otherwise, return the attachment of the mounted disk.
        THROW_HR_IF(WSL_E_USER_VHD_ALREADY_ATTACHED, found->second.Attachment.UserDisk);

        // Check if the attachment is still valid. It could be stale if the backing volume is reattached.
        if (wsl::windows::common::disk::IsBackingVolumeMounted(found->second.BackingFile.get()))
        {
            existingAttachment = found->second.Attachment;
            return;
        }

        schema::RemoveDiskWithAccess(
            m_system.get(),
            m_vmIdString.c_str(),
            found->second.Path.c_str(),
            found->second.Attachment.GuestAddress.Lun,
            found->second.Flags,
            static_cast<size_t>(found->second.DeviceTimeout.count()));

        m_attachedDisks.erase(found);
    });

    if (existingAttachment.has_value())
    {
        cleanup.release();
        return existingAttachment.value();
    }

    const auto lun = ReserveLunLocked(Request.Placement);

    wrapAttachFailure([&] {
        if (passThrough)
        {
            // Grant the VM access to the disk.
            schema::GrantVmWorkerProcessAccessToDisk(
                m_vmIdString.c_str(), path.c_str(), m_configuration.Description.Identity.UserToken.get());
            WI_SetFlag(diskFlags, wsl::windows::common::disk::DiskStateFlags::AccessGranted);

            // Set the disk offline if needed.
            //
            // N.B. The disk handle must be closed prior to adding the disk to the VM.
            if (wsl::windows::common::disk::TakeOffline(path.c_str(), timeoutMs))
            {
                WI_SetFlag(diskFlags, wsl::windows::common::disk::DiskStateFlags::Online);
            }

            // Add the disk to the VM.
            schema::AddPassThroughDiskWithRetry(m_system.get(), path.c_str(), lun, Request.ReadOnly, timeoutMs);
        }
        else
        {
            backingFile = wsl::windows::common::disk::OpenVhdBackingFile(path.c_str());

            schema::AddVhdWithAccess(
                m_system.get(),
                m_vmIdString.c_str(),
                path.c_str(),
                lun,
                Request.ReadOnly,
                m_configuration.Description.Identity.UserToken.get(),
                diskFlags);
        }
    });

    const VmDiskAttachment attachment{{m_configuration.Description.Identity, m_nextDiskId}, {0, lun}, Request.ReadOnly, Request.UserDisk};
    m_attachedDisks.emplace(
        attachment.Id.Value, AttachedDisk{attachment, passThrough, path, diskFlags, Request.DeviceTimeout, std::move(backingFile)});
    ++m_nextDiskId;
    cleanup.release();

    WSL_LOG(
        "HcsAttachDiskEnd",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(attachment.Id.Value, "diskId"),
        TraceLoggingValue(attachment.GuestAddress.Controller, "controller"),
        TraceLoggingValue(attachment.GuestAddress.Lun, "lun"),
        TraceLoggingValue(passThrough, "passThrough"),
        TraceLoggingValue(Request.ReadOnly, "readOnly"));

    return attachment;
}

void HcsVirtualMachineBackend::DetachDisk(VmDiskId Disk)
{
    ExecutionContext context(Context::DetachDisk);

    WSL_LOG(
        "HcsDetachDiskBegin",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Disk.Value, "diskId"));

    validation::ValidateResourceId(Disk, m_configuration.Description.Identity);

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system);

    // N.B. Unknown disks are reported with the same error as WslCoreVm::DetachDisk.
    const auto disk = m_attachedDisks.find(Disk.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), disk == m_attachedDisks.end());

    // Detach the disk from the VM and undo the host state changes that were performed to attach it.
    //
    // N.B. Volumes that the guest mounted from this disk must be unmounted by the caller before the
    //      disk is detached, since the guest protocol is not part of the backend.
    schema::RemoveDiskWithAccess(
        m_system.get(),
        m_vmIdString.c_str(),
        disk->second.Path.c_str(),
        disk->second.Attachment.GuestAddress.Lun,
        disk->second.Flags,
        static_cast<size_t>(disk->second.DeviceTimeout.count()));

    WSL_LOG(
        "HcsDetachDiskEnd",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Disk.Value, "diskId"),
        TraceLoggingValue(disk->second.Attachment.GuestAddress.Controller, "controller"),
        TraceLoggingValue(disk->second.Attachment.GuestAddress.Lun, "lun"),
        TraceLoggingValue(disk->second.PassThrough, "passThrough"));

    m_attachedDisks.erase(disk);
}

_Requires_lock_held_(m_lock)
void HcsVirtualMachineBackend::CloseFileSystemDevicesLocked() noexcept
{
    for (const auto& entry : m_fileSystemDevices)
    {
        if (entry.second.Plan9Server)
        {
            LOG_IF_FAILED(entry.second.Plan9Server->Teardown());
        }
    }

    // Device hosts must be shut down while the compute system and callback context still exist.
    m_guestDeviceManager.reset();
    m_fileSystemShares.clear();
    m_fileSystemDevices.clear();
}

VmFileSystemDevice HcsVirtualMachineBackend::CreateFileSystemDevice(const VmFileSystemDeviceRequest& Request)
{
    const auto tag = GetFileSystemDeviceTag(Request.Transport);
    WSL_LOG(
        "HcsCreateFileSystemDeviceBegin",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(tag.c_str(), "tag"));

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system || !m_guestDeviceManager);
    THROW_HR_IF(E_BOUNDS, m_nextDeviceId == UINT64_MAX);
    // Two devices conflict when the guest would reach them the same way: the same Plan9 socket port,
    // or the same tag. A socket device never conflicts with a tagged one.
    //
    // N.B. This compares the two transports through their addresses rather than visiting both
    //      variants at once, which would require a lambda for every pair of alternatives.
    const auto requestedPort = GetPlan9SocketPort(Request.Transport);
    for (const auto& entry : m_fileSystemDevices)
    {
        const auto existingPort = GetPlan9SocketPort(entry.second.Transport);
        const bool duplicate = requestedPort.has_value() || existingPort.has_value()
                                   ? requestedPort == existingPort
                                   : wsl::shared::string::IsEqual(tag, GetFileSystemDeviceTag(entry.second.Transport), false);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), duplicate);
    }

    VmFileSystemDevice device{{m_configuration.Description.Identity, m_nextDeviceId}, VmFileSystemDeviceState::Prepared};
    std::wstring mountOptions;
    wil::com_ptr<IPlan9FileSystem> plan9Server;
    std::optional<GUID> guestInstanceId;
    std::optional<GUID> registeredFileSystemClassId;
    auto removeOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        if (guestInstanceId.has_value())
        {
            m_guestDeviceManager->RemoveGuestDevice(guestInstanceId.value());
        }
        if (registeredFileSystemClassId.has_value())
        {
            m_guestDeviceManager->RemoveRemoteFileSystem(registeredFileSystemClassId.value(), tag);
        }
        if (plan9Server)
        {
            LOG_IF_FAILED(plan9Server->Teardown());
        }
    });

    std::visit(
        Overloaded{
            [&](const VmVirtioFsDevice& transport) {
                mountOptions = FormatVirtioFsMountOptions(transport.Options.MountOptions);

                // An aggregate device serves each of its shares as a child, so it can be created
                // before any share exists. A single-share device is created once its share is added.
                if (transport.Layout == VmVirtioFsLayout::Aggregate)
                {
                    const VirtioFsShareOptions options{.Kind = VirtiofsShareKind_Aggregate};
                    guestInstanceId = m_guestDeviceManager->AddVirtiofsDevice(
                        transport.Tag.c_str(), mountOptions.c_str(), L"", m_configuration.Description.Identity.UserToken.get(), options);
                    device.State = VmFileSystemDeviceState::Serving;
                }
            },
            [&](const VmPlan9VirtioDevice& transport) {
                THROW_HR_IF(E_INVALIDARG, !transport.ServerFactory);
                plan9Server = transport.ServerFactory(m_configuration.Description.Identity.UserToken.get());
                THROW_HR_IF(E_UNEXPECTED, !plan9Server);

                m_guestDeviceManager->AddRemoteFileSystem(transport.FileSystemClassId, transport.Tag.c_str(), plan9Server);
                registeredFileSystemClassId = transport.FileSystemClassId;
                guestInstanceId = m_guestDeviceManager->AddNewDevice(transport.DeviceType, plan9Server, transport.Tag.c_str());
                device.State = VmFileSystemDeviceState::Serving;
            },
            [&](const VmPlan9SocketDevice& transport) {
                THROW_HR_IF(E_INVALIDARG, !transport.ServerFactory);
                plan9Server = transport.ServerFactory(m_configuration.Description.Identity.UserToken.get());
                THROW_HR_IF(E_UNEXPECTED, !plan9Server);

                auto runAsUser = wil::impersonate_token(m_configuration.Description.Identity.UserToken.get());
                THROW_IF_FAILED(plan9Server->Init(&m_runtimeId, transport.Port.Value));
                THROW_IF_FAILED(plan9Server->Resume());
                device.State = VmFileSystemDeviceState::Serving;
            }},
        Request.Transport);

    device.GuestInstanceId = guestInstanceId;
    const auto inserted =
        m_fileSystemDevices
            .emplace(device.Id.Value, FileSystemDevice{device, Request.Transport, std::move(mountOptions), std::move(plan9Server)})
            .second;
    WI_ASSERT(inserted);
    ++m_nextDeviceId;
    removeOnFailure.release();

    WSL_LOG(
        "HcsCreateFileSystemDeviceEnd",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(device.Id.Value, "deviceId"),
        TraceLoggingValue(tag.c_str(), "tag"),
        TraceLoggingValue(device.GuestInstanceId.has_value(), "created"));

    return device;
}

HcsVirtualMachineBackend::FileSystemDeviceMap::iterator HcsVirtualMachineBackend::FindFileSystemDeviceLocked(VmDeviceId Device)
{
    const auto device = m_fileSystemDevices.find(Device.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), device == m_fileSystemDevices.end());

    return device;
}

const HcsVirtualMachineBackend::FileSystemShare* HcsVirtualMachineBackend::FindFileSystemShareLocked(
    VmDeviceId Device, const std::wstring& HostPath, const std::wstring& MountOptions) const
{
    for (const auto& entry : m_fileSystemShares)
    {
        const auto& share = entry.second;
        if ((share.Share.Device.Value == Device.Value) && (share.Share.EffectiveHostPath.native() == HostPath) &&
            (share.MountOptions == MountOptions))
        {
            return &share;
        }
    }

    return nullptr;
}

HANDLE HcsVirtualMachineBackend::ResolveShareUserToken(const VmFileSystemShareRequest& Request) const
{
    HANDLE userToken = Request.UserToken ? Request.UserToken.get() : m_configuration.Description.Identity.UserToken.get();
    THROW_HR_IF_MSG(E_UNEXPECTED, !userToken, "UserToken not set for the file system share request");

    return userToken;
}

std::wstring HcsVirtualMachineBackend::AddPlan9ShareLocked(
    const FileSystemDevice& Device, const VmFileSystemShareRequest& Request, HANDLE UserToken, const std::wstring& HostPath) const
{
    const auto* options = std::get_if<VmPlan9ShareOptions>(&Request.Options);
    THROW_HR_IF_MSG(E_INVALIDARG, !options, "A Plan 9 device requires Plan 9 share options");
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !Device.Plan9Server);

    auto flags = schema::Plan9ShareFlags::None;
    WI_SetFlagIf(flags, schema::Plan9ShareFlags::ReadOnly, Request.ReadOnly);
    WI_SetFlagIf(flags, schema::Plan9ShareFlags::LinuxMetadata, options->LinuxMetadata);
    WI_SetFlagIf(flags, schema::Plan9ShareFlags::CaseSensitive, options->CaseSensitive);
    WI_SetFlagIf(flags, schema::Plan9ShareFlags::UseShareRootIdentity, options->UseShareRootIdentity);
    WI_SetFlagIf(flags, schema::Plan9ShareFlags::AllowOptions, options->AllowOptions);
    WI_SetFlagIf(flags, schema::Plan9ShareFlags::AllowSubPaths, options->AllowSubPaths);

    // Allow the Plan 9 server to create NT symlinks.
    //
    // N.B. This may fail for unelevated users, however symlink creation will
    //      succeed even without this privilege if developer mode is enabled.
    wsl::windows::common::security::EnableTokenPrivilege(UserToken, SE_CREATE_SYMBOLIC_LINK_NAME);

    auto accessName = Request.Name.empty() ? GenerateShareName() : Request.Name;
    auto runAsUser = wil::impersonate_token(UserToken);
    AddPlan9SharePath(Device.Plan9Server, accessName.c_str(), HostPath.c_str(), static_cast<UINT32>(flags));

    return accessName;
}

_Requires_lock_held_(m_lock)
void HcsVirtualMachineBackend::RemovePlan9ShareLocked(const FileSystemDevice& Device, const std::wstring& AccessName, HANDLE UserToken) const
{
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !Device.Plan9Server);

    auto runAsUser = wil::impersonate_token(UserToken);
    THROW_IF_FAILED(Device.Plan9Server->RemoveShare(AccessName.c_str()));
}

VmFileSystemShare HcsVirtualMachineBackend::AddFileSystemShare(VmDeviceId Device, const VmFileSystemShareRequest& Request)
{
    validation::ValidateResourceId(Device, m_configuration.Description.Identity);
    THROW_HR_IF_MSG(E_INVALIDARG, Request.HostPath.empty(), "A host path is required");
    const auto userToken = ResolveShareUserToken(Request);

    WSL_LOG(
        "HcsAddFileSystemShareBegin",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(Request.HostPath.c_str(), "path"),
        TraceLoggingValue(Request.ReadOnly, "readOnly"));

    auto lock = m_lock.lock_exclusive();
    // N.B. A torn-down VM is reported as HCS_E_TERMINATED to match WslCoreVm::AddDrvFsShare.
    THROW_HR_IF(HCS_E_TERMINATED, !m_system || !m_guestDeviceManager);
    THROW_HR_IF(E_BOUNDS, m_nextShareId == std::numeric_limits<std::uint64_t>::max());
    const auto device = FindFileSystemDeviceLocked(Device);

    // A virtio-fs share resolves to a canonical directory, while a Plan 9 share may be rooted at a
    // prefix such as '\\?' so that the guest can mount arbitrary subpaths below it.
    std::wstring hostPath;
    std::wstring mountOptions;
    std::optional<VmFileSystemShare> reused;
    VmFileSystemShareAddress guestAddress;
    std::visit(
        Overloaded{
            [&](const VmVirtioFsDevice& transport) {
                const auto* options = std::get_if<VmVirtioFsShareOptions>(&Request.Options);
                THROW_HR_IF_MSG(E_INVALIDARG, !options, "A virtio-fs device requires virtio-fs share options");

                hostPath = NormalizeSharePath(Request.HostPath);

                auto shareOptions = options->MountOptions;
                if (Request.ReadOnly)
                {
                    shareOptions[L"ro"] = {};
                }

                mountOptions = FormatVirtioFsMountOptions(shareOptions);

                // Repeating a request for the same path and options reuses the existing share so that
                // multiple guest mounts of one host directory are backed by a single virtio-fs share.
                if (const auto* existing = FindFileSystemShareLocked(Device, hostPath, mountOptions))
                {
                    reused = existing->Share;
                    return;
                }

                std::optional<std::wstring> childName;
                if (transport.Layout == VmVirtioFsLayout::Aggregate)
                {
                    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !device->second.Device.GuestInstanceId.has_value());
                    childName = Request.Name.empty() ? GenerateShareName() : Request.Name;
                    m_guestDeviceManager->AddVirtiofsChild(
                        device->second.Device.GuestInstanceId.value(), childName->c_str(), mountOptions.c_str(), hostPath.c_str());
                }
                else
                {
                    // A single-share device is the share, so it is created on first use and cannot be shared further.
                    THROW_HR_IF_MSG(
                        E_INVALIDARG, !Request.Name.empty(), "A single-share virtio-fs device does not accept a share name");
                    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), device->second.Device.GuestInstanceId.has_value());
                    device->second.Device.GuestInstanceId =
                        m_guestDeviceManager->AddVirtiofsDevice(transport.Tag.c_str(), mountOptions.c_str(), hostPath.c_str(), userToken);
                }

                device->second.Device.State = VmFileSystemDeviceState::Serving;
                guestAddress = VmVirtioFsShareAddress{transport.Tag, std::move(childName)};
            },
            [&](const VmPlan9SocketDevice& transport) {
                hostPath = Request.HostPath.native();
                guestAddress = VmPlan9SocketShareAddress{transport.Port, AddPlan9ShareLocked(device->second, Request, userToken, hostPath)};
            },
            [&](const VmPlan9VirtioDevice& transport) {
                hostPath = Request.HostPath.native();
                guestAddress = VmPlan9VirtioShareAddress{transport.Tag, AddPlan9ShareLocked(device->second, Request, userToken, hostPath)};
            }},
        device->second.Transport);

    if (reused.has_value())
    {
        WSL_LOG(
            "HcsAddFileSystemShareEnd",
            TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
            TraceLoggingValue(Device.Value, "deviceId"),
            TraceLoggingValue(reused->Id.Value, "shareId"),
            TraceLoggingValue(false, "created"));

        return reused.value();
    }

    VmFileSystemShare share{};
    share.Id = VmShareId{m_configuration.Description.Identity, m_nextShareId};
    share.Device = Device;
    share.GuestAddress = std::move(guestAddress);
    share.EffectiveHostPath = hostPath;
    share.ReadOnly = Request.ReadOnly;

    const auto inserted =
        m_fileSystemShares.emplace(share.Id.Value, FileSystemShare{share, std::move(mountOptions), Request.UserToken}).second;
    WI_ASSERT(inserted);
    ++m_nextShareId;

    WSL_LOG(
        "HcsAddFileSystemShareEnd",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(share.Id.Value, "shareId"),
        TraceLoggingValue(true, "created"));

    return share;
}

void HcsVirtualMachineBackend::RemoveFileSystemShare(VmShareId Share)
{
    WSL_LOG(
        "HcsRemoveFileSystemShareBegin",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Share.Value, "shareId"));

    validation::ValidateResourceId(Share, m_configuration.Description.Identity);

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system || !m_guestDeviceManager);
    const auto share = m_fileSystemShares.find(Share.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), share == m_fileSystemShares.end());
    const auto device = m_fileSystemDevices.find(share->second.Share.Device.Value);
    THROW_HR_IF(E_UNEXPECTED, device == m_fileSystemDevices.end());

    // A Plan 9 share is removed under the identity that added it, matching AddPlan9ShareLocked.
    HANDLE userToken = share->second.UserToken ? share->second.UserToken.get() : m_configuration.Description.Identity.UserToken.get();
    THROW_HR_IF_MSG(E_UNEXPECTED, !userToken, "UserToken not set for the file system share");

    // A Plan 9 device serves each of its shares by name, so removing one leaves the device serving
    // the others. A virtio-fs share is either a child of an aggregate device or the device itself.
    std::visit(
        Overloaded{
            [&](const VmVirtioFsDevice& transport) {
                THROW_HR_IF(E_UNEXPECTED, !device->second.Device.GuestInstanceId.has_value());

                const auto& guestAddress = std::get<VmVirtioFsShareAddress>(share->second.Share.GuestAddress);
                if (transport.Layout == VmVirtioFsLayout::Aggregate)
                {
                    THROW_HR_IF(E_UNEXPECTED, !guestAddress.ChildName.has_value());
                    m_guestDeviceManager->RemoveVirtiofsChild(
                        device->second.Device.GuestInstanceId.value(), guestAddress.ChildName->c_str());
                }
                else
                {
                    THROW_HR_IF(E_UNEXPECTED, guestAddress.ChildName.has_value());
                    m_guestDeviceManager->RemoveGuestDevice(device->second.Device.GuestInstanceId.value());
                    device->second.Device.GuestInstanceId.reset();
                    device->second.Device.State = VmFileSystemDeviceState::Prepared;
                }
            },
            [&](const VmPlan9SocketDevice&) {
                const auto& guestAddress = std::get<VmPlan9SocketShareAddress>(share->second.Share.GuestAddress);
                RemovePlan9ShareLocked(device->second, guestAddress.AccessName, userToken);
            },
            [&](const VmPlan9VirtioDevice&) {
                const auto& guestAddress = std::get<VmPlan9VirtioShareAddress>(share->second.Share.GuestAddress);
                RemovePlan9ShareLocked(device->second, guestAddress.AccessName, userToken);
            }},
        device->second.Transport);

    WSL_LOG(
        "HcsRemoveFileSystemShareEnd",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Share.Value, "shareId"),
        TraceLoggingValue(device->second.Device.Id.Value, "deviceId"),
        TraceLoggingValue(GetFileSystemDeviceTag(device->second.Transport).c_str(), "tag"));

    m_fileSystemShares.erase(share);
}

VmNetworkAttachment HcsVirtualMachineBackend::AddNetworkAdapter(const VmNetworkAdapterRequest&)
{
    THROW_HR(c_notSupported);
}

VmPortBinding HcsVirtualMachineBackend::BindPort(VmDeviceId, const VmPortBindingRequest&)
{
    THROW_HR(c_notSupported);
}

void HcsVirtualMachineBackend::UnbindPort(VmPortBindingId)
{
    THROW_HR(c_notSupported);
}

void CALLBACK HcsVirtualMachineBackend::OnSystemEvent(HCS_EVENT* Event, void* Context) noexcept
try
{
    auto* backend = static_cast<HcsVirtualMachineBackend*>(Context);
    if (Event->Type == HcsEventSystemCrashInitiated || Event->Type == HcsEventSystemCrashReport)
    {
        backend->OnCrash(Event->EventData);
    }
    else if (Event->Type == HcsEventSystemExited || Event->Type == HcsEventServiceDisconnect)
    {
        backend->OnExit(Event->EventData);
    }
}
CATCH_LOG();

void HcsVirtualMachineBackend::OnCrash(PCWSTR Details)
{
    if (m_vmCrashEvent.is_signaled())
    {
        return;
    }

    WSL_LOG("GuestCrash", TraceLoggingValue(Details, "Data"));
    const auto crashInformation = wsl::shared::FromJson<wsl::windows::common::hcs::CrashReport>(Details);

    if (m_crashCapture)
    {
        m_vmCrashLogFile = wsl::windows::common::hcs::WriteVmCrashLog(
            m_crashCapture->Path,
            m_crashCapture->MaxCrashLogCount,
            m_configuration.Description.Identity.VmId,
            m_configuration.Description.Identity.UserToken.get(),
            crashInformation.CrashLog);
    }

    m_vmCrashEvent.SetEvent();
}

void HcsVirtualMachineBackend::OnExit(PCWSTR ExitDetails)
{
    // Closing the system drains callbacks before their event and context are destroyed.
    // An exit without a prior termination request must cancel pending operations.
    {
        auto exitDetailsLock = m_exitDetailsLock.lock_exclusive();
        if (ExitDetails != nullptr)
        {
            m_exitDetails = ExitDetails;
        }
    }

    m_exitEvent.SetEvent();

    if (!m_terminatingEvent.is_signaled())
    {
        WSL_LOG("AbnormalVmExit", TraceLoggingValue(ExitDetails, "Details"));
        m_terminatingEvent.SetEvent();
    }

    NotifyTerminated(m_configuration.Description.Identity);
}