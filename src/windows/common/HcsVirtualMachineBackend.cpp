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
#include "WslCoreNetworkEndpointSettings.h"
#include "hvsocket.hpp"

using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::common::vm::c_maximumDisks;
using wsl::windows::common::vm::c_mib;
using wsl::windows::common::vm::c_notSupported;

namespace validation = wsl::windows::common::vm::validation;

namespace {

namespace schema = wsl::windows::common::hcs;

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
            [](const VmPlan9SocketDevice&) { return std::wstring{}; },
            [](const VmPlan9HostedDevice&) { return std::wstring{}; }},
        Transport);
}

// Plan9 socket devices are addressed by the port their server listens on.
std::optional<std::uint32_t> GetPlan9SocketPort(const VmFileSystemDeviceTransport& Transport)
{
    return std::visit(
        Overloaded{
            [](const VmPlan9SocketDevice& transport) { return std::optional<std::uint32_t>{transport.Port.Value}; },
            [](const VmPlan9HostedDevice& transport) { return std::optional<std::uint32_t>{transport.Port.Value}; },
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

schema::Plan9ShareFlags GetPlan9ShareFlags(const VmFileSystemShareRequest& Request)
{
    const auto* options = std::get_if<VmPlan9ShareOptions>(&Request.Options);
    THROW_HR_IF_MSG(E_INVALIDARG, !options, "A Plan 9 device requires Plan 9 share options");

    auto flags = schema::Plan9ShareFlags::None;
    WI_SetFlagIf(flags, schema::Plan9ShareFlags::ReadOnly, Request.ReadOnly);
    WI_SetFlagIf(flags, schema::Plan9ShareFlags::LinuxMetadata, options->LinuxMetadata);
    WI_SetFlagIf(flags, schema::Plan9ShareFlags::CaseSensitive, options->CaseSensitive);
    WI_SetFlagIf(flags, schema::Plan9ShareFlags::UseShareRootIdentity, options->UseShareRootIdentity);
    WI_SetFlagIf(flags, schema::Plan9ShareFlags::AllowOptions, options->AllowOptions);
    WI_SetFlagIf(flags, schema::Plan9ShareFlags::AllowSubPaths, options->AllowSubPaths);
    return flags;
}

bool AreSameTokens(HANDLE First, HANDLE Second)
{
    if (First == Second)
    {
        return true;
    }

    const auto first = wil::get_token_information<TOKEN_STATISTICS>(First);
    const auto second = wil::get_token_information<TOKEN_STATISTICS>(Second);
    return first.TokenId.HighPart == second.TokenId.HighPart && first.TokenId.LowPart == second.TokenId.LowPart;
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

VmEffectiveMemory ConfigureMemory(const VmMemoryRequest& Request, const VmMmioRequest& Mmio, schema::Memory& Settings)
{
    VmEffectiveMemory memory{};
    memory.SizeBytes = (Request.SizeBytes / c_mib) * c_mib;
    memory.AllowOvercommit = validation::ValidateFeature(Request.AllowOvercommit, L"memory overcommit", true);
    memory.DeferredCommit = validation::ValidateFeature(Request.DeferredCommit, L"deferred memory commit", true);
    memory.ColdDiscard = validation::ValidateFeature(Request.ColdDiscard, L"cold discard", true);
    memory.SmallPageBacking =
        validation::ValidateFeature(Request.SmallPageBacking, L"small-page memory", schema::IsSmallPageMemorySupported());
    THROW_HR_IF(
        E_INVALIDARG,
        (memory.SmallPageBacking || Request.FaultClusterSizeShift.has_value() || Request.DirectMapFaultClusterSizeShift.has_value()) &&
            !memory.AllowOvercommit);
    memory.FaultClusterSizeShift = Request.FaultClusterSizeShift;
    memory.DirectMapFaultClusterSizeShift = Request.DirectMapFaultClusterSizeShift;
    memory.PageReportingOrder = Request.PageReportingOrder;
    memory.HostingProcessNameSuffix = Request.HostingProcessNameSuffix;

    Settings.SizeInMB = memory.SizeBytes / c_mib;
    Settings.AllowOvercommit = memory.AllowOvercommit;
    Settings.EnableDeferredCommit = memory.DeferredCommit;
    Settings.EnableColdDiscardHint = memory.ColdDiscard;
    if (memory.SmallPageBacking)
    {
        Settings.BackingPageSize = schema::MemoryBackingPageSize::Small;
    }
    Settings.FaultClusterSizeShift = memory.FaultClusterSizeShift;
    Settings.DirectMapFaultClusterSizeShift = memory.DirectMapFaultClusterSizeShift;
    Settings.HostingProcessNameSuffix = memory.HostingProcessNameSuffix;

    if (Mmio.HighWindowSizeBytes != 0)
    {
        THROW_HR_IF(E_INVALIDARG, (Mmio.HighWindowSizeBytes % c_mib) != 0);
        Settings.HighMmioGapInMB = Mmio.HighWindowSizeBytes / c_mib;
        memory.HighMmioSizeBytes = Mmio.HighWindowSizeBytes;
        if (Mmio.MaximumGuestAddressBits)
        {
            THROW_HR_IF(
                E_INVALIDARG,
                Mmio.MaximumGuestAddressBits.value() >= 64 ||
                    Mmio.HighWindowSizeBytes > (UINT64{1} << Mmio.MaximumGuestAddressBits.value()));
            memory.HighMmioBaseBytes = (UINT64{1} << Mmio.MaximumGuestAddressBits.value()) - Mmio.HighWindowSizeBytes;
            THROW_HR_IF(E_INVALIDARG, (memory.HighMmioBaseBytes.value() % c_mib) != 0);
            Settings.HighMmioBaseInMB = memory.HighMmioBaseBytes.value() / c_mib;
        }
    }
    else
    {
        THROW_HR_IF(E_INVALIDARG, Mmio.MaximumGuestAddressBits.has_value());
    }

    return memory;
}

// Maps console requests onto the HCS devices that serve them. A serial console is served by a COM
// port and a virtio console by a port on the virtio-serial controller.
//
// N.B. The ordering of virtio ports is significant because it determines the order the guest
//      enumerates them as /dev/hvc devices, so ports are addressed by the caller-supplied index.
void ConfigureConsoles(const std::vector<VmConsoleRequest>& Requests, schema::Devices& Settings)
{
    for (const auto& request : Requests)
    {
        std::visit(
            Overloaded{
                [&](const VmSerialConsole& console) {
                    THROW_HR_IF(E_INVALIDARG, console.NamedPipe.empty());
                    const auto port = std::to_string(console.Port);
                    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), Settings.ComPorts.contains(port));
                    Settings.ComPorts[port] = schema::ComPort{console.NamedPipe.native()};
                },
                [&](const VmVirtioConsole& console) {
                    THROW_HR_IF(E_INVALIDARG, console.NamedPipe.empty() || console.GuestName.empty());
                    if (!Settings.VirtioSerial.has_value())
                    {
                        Settings.VirtioSerial.emplace();
                    }

                    const auto port = std::to_string(console.Port);
                    auto& ports = Settings.VirtioSerial->Ports;
                    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), ports.contains(port));

                    // The guest addresses a virtio console by name, so names must be unique.
                    THROW_HR_IF(
                        HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
                        std::any_of(ports.begin(), ports.end(), [&](const auto& entry) {
                            return entry.second.Name == console.GuestName;
                        }));

                    ports[port] = schema::VirtioSerialPort{console.GuestName, console.NamedPipe.native(), true};
                }},
            request.Device);
    }
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

    VmConfiguration configuration{};
    configuration.Settings.Owner = Request.Owner;
    configuration.Settings.ShouldTerminateOnLastHandleClosed = true;
    configuration.Settings.SchemaVersion.Major = 2;
    configuration.Settings.SchemaVersion.Minor = wsl::windows::common::helpers::IsWindows11OrAbove() ? 7 : 3;
    configuration.Settings.VirtualMachine.StopOnReset = true;
    auto& description = configuration.Description;
    description.Identity = Request.Identity;
    description.Backend = BackendKind::Hcs;
    description.Processor = ConfigureProcessor(Request.Processor, configuration.Settings.VirtualMachine.ComputeTopology.Processor);
    description.Memory = ConfigureMemory(Request.Memory, Request.Mmio, configuration.Settings.VirtualMachine.ComputeTopology.Memory);
    description.Boot = ConfigureBoot(Request.Boot, configuration.Settings.VirtualMachine.Chipset);
    description.Boot.Consoles = Request.Consoles;
    ConfigureConsoles(Request.Consoles, configuration.Settings.VirtualMachine.Devices);
    auto& scsi = configuration.Settings.VirtualMachine.Devices.Scsi["0"];
    scsi = {};

    // Attach the disks the VM boots from up front so that the guest can reach them without waiting
    // for a hot add. Their LUNs are reported so that the caller can name them in the guest.
    const auto vmIdString = wsl::shared::string::GuidToString<wchar_t>(Request.Identity.VmId, wsl::shared::string::GuidToStringFlags::None);
    std::uint32_t nextLun = 0;
    for (const auto& bootDisk : Request.BootDisks)
    {
        THROW_HR_IF(E_INVALIDARG, bootDisk.Key.empty());
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), description.BootDisks.contains(bootDisk.Key));
        THROW_HR_IF_MSG(
            c_notSupported, !std::holds_alternative<VmVirtualDiskSource>(bootDisk.Disk.Source), "HCS boot disks must be virtual disks");

        const auto& path = validation::ValidateDiskSource(bootDisk.Disk);
        std::uint32_t lun = nextLun;
        if (bootDisk.Disk.Placement)
        {
            THROW_HR_IF(c_notSupported, bootDisk.Disk.Placement->Address.Controller != 0);
            lun = bootDisk.Disk.Placement->Address.Lun;
        }

        THROW_HR_IF(E_BOUNDS, lun >= c_maximumDisks);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), scsi.Attachments.contains(std::to_string(lun)));
        nextLun = lun + 1;

        // Best effort: failures (for instance no WRITE_DAC on a SYSTEM-owned VHD) are swallowed
        // since the VM worker process may already have access via inherited ACLs; otherwise
        // starting the VM surfaces E_ACCESSDENIED.
        wsl::windows::common::disk::DiskStateFlags diskFlags{};
        if (bootDisk.GrantHostAccess)
        {
            try
            {
                auto runAsUser = wil::impersonate_token(Request.Identity.UserToken.get());
                schema::GrantVmAccess(vmIdString.c_str(), path.c_str());
                WI_SetFlag(diskFlags, wsl::windows::common::disk::DiskStateFlags::AccessGranted);
            }
            CATCH_LOG()
        }

        auto backingFile = wsl::windows::common::disk::OpenVhdBackingFile(path.c_str());

        schema::Attachment attachment{};
        attachment.Type = schema::AttachmentType::VirtualDisk;
        attachment.Path = path;
        attachment.ReadOnly = bootDisk.Disk.ReadOnly;
        attachment.SupportCompressedVolumes = true;
        attachment.AlwaysAllowSparseFiles = true;
        attachment.SupportEncryptedFiles = true;
        scsi.Attachments[std::to_string(lun)] = std::move(attachment);

        const VmDiskAttachment diskAttachment{
            {Request.Identity, configuration.NextDiskId}, {0, lun}, bootDisk.Disk.ReadOnly, bootDisk.Disk.UserDisk, path, false};
        description.BootDisks.emplace(bootDisk.Key, diskAttachment);
        configuration.BootDisks.emplace(
            diskAttachment.Id.Value, AttachedDisk{diskAttachment, {diskFlags, bootDisk.Disk.DeviceTimeout, std::move(backingFile)}});

        ++configuration.NextDiskId;
    }

    if (Request.CrashCapture && Request.CrashCapture->SavedStateFolder)
    {
        THROW_HR_IF(E_INVALIDARG, Request.CrashCapture->SavedStateFolder->empty());

        // Saved-state capture is configured through the DebugOptions of schema 2.7, so it is left
        // unconfigured on Windows 10 just as WslCoreVm does.
        if (wsl::windows::common::helpers::IsWindows11OrAbove())
        {
            const auto savedStatePath = schema::CreateVmSavedStateFile(
                Request.CrashCapture->SavedStateFolder.value(), Request.Identity.VmId, Request.Identity.UserToken.get());
            configuration.Settings.VirtualMachine.DebugOptions.BugcheckSavedStateFileName = savedStatePath.native();
            m_vmSavedStateFile = savedStatePath;
        }
        else
        {
            THROW_HR_IF_MSG(
                c_notSupported,
                Request.CrashCapture->Policy == VmSelectionPolicy::Required,
                "Saved-state crash capture requires Windows 11 or above");
        }
    }

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
    std::shared_ptr<GuestDeviceManager> guestDeviceManager;
    schema::unique_hcs_system system;
    std::map<std::uint64_t, AttachedDisk> attachedDisks;
    {
        auto lock = m_lock.lock_exclusive();
        CloseNetworkAdaptersLocked();
        guestDeviceManager = std::move(m_guestDeviceManager);
        CloseGuestDevicesLocked();
        system = std::move(m_system);
        attachedDisks = std::move(m_attachedDisks);
        CloseGuestListenersLocked(m_configuration.Description.Identity);
    }

    if (system)
    {
        LOG_IF_FAILED(wil::ResultFromException([&] { schema::TerminateComputeSystem(system.get()); }));
    }
    system.reset();
    guestDeviceManager.reset();
    CleanupAttachedDisks(std::move(attachedDisks));

    try
    {
        auto crashLock = m_crashInformationLock.lock_shared();
        auto runAsUser = wil::impersonate_token(m_configuration.Description.Identity.UserToken.get());
        if (m_vmSavedStateFile && std::filesystem::exists(m_vmSavedStateFile.value()) &&
            std::filesystem::is_empty(m_vmSavedStateFile.value()))
        {
            std::filesystem::remove(m_vmSavedStateFile.value());
        }
    }
    CATCH_LOG()

    auto terminationInformationLock = m_terminationInformationLock.lock_shared();
    WSL_LOG(
        "HcsVirtualMachineBackendDestroyed",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(m_exitEvent.is_signaled(), "exitEventSignaled"),
        TraceLoggingValue(m_terminationInformation.Details.c_str(), "exitDetails"));
}

std::unique_ptr<HcsVirtualMachineBackend> HcsVirtualMachineBackend::Create(const VmCreateRequest& Request)
{
    const auto context = CreateExecutionContext(Context::CreateVm);
    auto newInstance = std::unique_ptr<HcsVirtualMachineBackend>{new HcsVirtualMachineBackend{}};
    try
    {
        newInstance->m_configuration.Description.Identity = Request.Identity;
        newInstance->m_configuration.Description.Backend = BackendKind::Hcs;
        if (Request.CrashCapture)
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
                auto crashLock = newInstance->m_crashInformationLock.lock_shared();
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
    auto bootDisks = std::move(configuration.BootDisks);
    const auto nextDiskId = configuration.NextDiskId;
    m_configuration = std::move(configuration);
    m_vmIdString = id;
    auto lock = m_lock.lock_exclusive();

    // Track the boot disks before the compute system is created so that the host state changes made
    // to attach them are undone even if creation fails.
    m_attachedDisks = std::move(bootDisks);
    m_nextDiskId = nextDiskId;
    m_system = schema::CreateComputeSystem(id.c_str(), settings.c_str());
    m_state = VmState::Created;
    m_runtimeId = wsl::windows::common::hcs::GetRuntimeId(m_system.get());
    m_guestDeviceManager = std::make_shared<GuestDeviceManager>(id, m_runtimeId, Request.EnableTelemetry);
    m_pendingNetworkAdapters = Request.NetworkAdapters;
    schema::RegisterCallback(m_system.get(), OnSystemEvent, this);
}

VmPlatformCapabilities HcsVirtualMachineBackend::GetCapabilities() const
{
    return QueryCapabilities();
}

HCS_SYSTEM HcsVirtualMachineBackend::GetComputeSystemHandle() const
{
    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system);
    return m_system.get();
}

std::shared_ptr<GuestDeviceManager> HcsVirtualMachineBackend::GetGuestDeviceManager() const
{
    auto lock = m_lock.lock_shared();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system || !m_guestDeviceManager);
    return m_guestDeviceManager;
}

VmPlatformCapabilities HcsVirtualMachineBackend::QueryCapabilities()
{
    VmPlatformCapabilities capabilities{};
    capabilities.Backend = BackendKind::Hcs;
    for (const auto feature :
         {VmFeature::NestedVirtualization,
          VmFeature::DeferredMemoryCommit,
          VmFeature::ColdDiscard,
          VmFeature::SerialConsole,
          VmFeature::VirtioConsole,
          VmFeature::PhysicalDisk,
          VmFeature::VirtioFsFileBacked,
          VmFeature::UserModeNatNetwork,
          VmFeature::TcpPortBinding,
          VmFeature::UdpPortBinding,
          VmFeature::Ipv6PortBinding})
    {
        capabilities.Features.set(static_cast<size_t>(feature));
    }

    capabilities.Features.set(static_cast<size_t>(VmFeature::NestedVirtualization), schema::IsNestedVirtualizationSupported());
    // Saved-state crash capture needs the DebugOptions of schema 2.7, which Windows 10 does not have.
    capabilities.Features.set(static_cast<size_t>(VmFeature::SavedStateOnCrash), wsl::windows::common::helpers::IsWindows11OrAbove());
    const auto [perfmonPmuSupported, perfmonLbrSupported] = schema::GetPerfmonCapabilities();
    capabilities.Features.set(static_cast<size_t>(VmFeature::PerfmonPmu), perfmonPmuSupported);
    capabilities.Features.set(static_cast<size_t>(VmFeature::PerfmonLbr), perfmonLbrSupported);
    return capabilities;
}

VmDescription HcsVirtualMachineBackend::GetDescription() const
{
    auto lock = m_lock.lock_shared();
    return m_configuration.Description;
}

VmState HcsVirtualMachineBackend::GetState() const
{
    auto lock = m_lock.lock_shared();
    return m_state;
}

VmTerminationInformation HcsVirtualMachineBackend::GetTerminationReason() const
{
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_exitEvent.is_signaled());
    auto lock = m_terminationInformationLock.lock_shared();
    return m_terminationInformation;
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
    std::vector<VmNetworkAdapterRequest> networkAdapters;
    {
        auto lock = m_lock.lock_exclusive();
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system);
        const auto settings = wsl::shared::ToJsonW(m_configuration.Settings);
        schema::StartComputeSystem(m_system.get(), settings.c_str());
        m_state = VmState::Running;
        networkAdapters = std::move(m_pendingNetworkAdapters);
    }

    for (const auto& adapter : networkAdapters)
    {
        AddNetworkAdapter(adapter);
    }
}

void HcsVirtualMachineBackend::Terminate()
{
    // AddPersistentMemory releases m_lock while the device host attaches the device and the caller
    // waits for it. Serialize termination with that entire lifecycle.
    auto persistentMemoryLock = m_persistentMemoryLock.lock_exclusive();
    std::shared_ptr<GuestDeviceManager> guestDeviceManager;
    schema::unique_hcs_system system;
    std::map<std::uint64_t, AttachedDisk> attachedDisks;
    {
        auto lock = m_lock.lock_exclusive();
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system);
        CloseNetworkAdaptersLocked();
        guestDeviceManager = std::move(m_guestDeviceManager);
        CloseGuestDevicesLocked();
        system = std::move(m_system);
        m_state = VmState::Stopped;
        attachedDisks = std::move(m_attachedDisks);
        CloseGuestListenersLocked(m_configuration.Description.Identity);
    }

    auto cleanup = wil::scope_exit([&] {
        system.reset();
        guestDeviceManager.reset();
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
            if (entry.second.GuestAddress.Lun == Lun)
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
        if (WI_IsFlagSet(disk.Backend.Flags, wsl::windows::common::disk::DiskStateFlags::AccessGranted))
        {
            try
            {
                schema::RevokeVmAccess(m_vmIdString.c_str(), disk.Path.c_str());
            }
            CATCH_LOG()
        }

        if (WI_IsFlagSet(disk.Backend.Flags, wsl::windows::common::disk::DiskStateFlags::Online))
        {
            try
            {
                wsl::windows::common::disk::BringOnline(disk.Path.c_str(), static_cast<size_t>(disk.Backend.DeviceTimeout.count()));
            }
            CATCH_LOG()
        }
    }
}

VmDiskAttachment HcsVirtualMachineBackend::AttachDisk(const VmDiskRequest& Request)
{
    const auto context = CreateExecutionContext(Context::MountDisk);

    const auto& path = validation::ValidateDiskSource(Request);
    const bool passThrough = std::holds_alternative<VmPhysicalDiskSource>(Request.Source);
    const auto timeoutMs = static_cast<size_t>(Request.DeviceTimeout.count());
    HANDLE userToken = Request.UserToken ? Request.UserToken.get() : m_configuration.Description.Identity.UserToken.get();
    THROW_HR_IF_MSG(E_UNEXPECTED, !userToken, "UserToken not set for the disk request");

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
        THROW_HR_IF(WSL_E_USER_VHD_ALREADY_ATTACHED, found->second.UserDisk);

        // Check if the attachment is still valid. It could be stale if the backing volume is reattached.
        if (wsl::windows::common::disk::IsBackingVolumeMounted(found->second.Backend.BackingFile.get()))
        {
            existingAttachment = found->second;
            return;
        }

        schema::RemoveDiskWithAccess(
            m_system.get(),
            m_vmIdString.c_str(),
            found->second.Path.c_str(),
            found->second.GuestAddress.Lun,
            found->second.Backend.Flags,
            static_cast<size_t>(found->second.Backend.DeviceTimeout.count()));

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
                m_vmIdString.c_str(), path.c_str(), userToken);
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
                userToken,
                diskFlags);
        }
    });

    const VmDiskAttachment attachment{
        {m_configuration.Description.Identity, m_nextDiskId}, {0, lun}, Request.ReadOnly, Request.UserDisk, path, passThrough};
    m_attachedDisks.emplace(attachment.Id.Value, AttachedDisk{attachment, {diskFlags, Request.DeviceTimeout, std::move(backingFile)}});
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

std::vector<VmDiskAttachment> HcsVirtualMachineBackend::GetAttachedDisks() const
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

void HcsVirtualMachineBackend::DetachDisk(VmDiskId Disk)
{
    const auto context = CreateExecutionContext(Context::DetachDisk);

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
        disk->second.GuestAddress.Lun,
        disk->second.Backend.Flags,
        static_cast<size_t>(disk->second.Backend.DeviceTimeout.count()));

    WSL_LOG(
        "HcsDetachDiskEnd",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Disk.Value, "diskId"),
        TraceLoggingValue(disk->second.GuestAddress.Controller, "controller"),
        TraceLoggingValue(disk->second.GuestAddress.Lun, "lun"),
        TraceLoggingValue(disk->second.PassThrough, "passThrough"));

    m_attachedDisks.erase(disk);
}

_Requires_lock_held_(m_lock)
void HcsVirtualMachineBackend::CloseGuestDevicesLocked() noexcept
{
    for (const auto& entry : m_fileSystemDevices)
    {
        if (entry.second.Backend.Plan9Server)
        {
            LOG_IF_FAILED(entry.second.Backend.Plan9Server->Teardown());
        }
    }

    // The caller retains the device manager until HCS has finished with its hosts.
    m_fileSystemShares.clear();
    m_fileSystemDevices.clear();
    m_persistentMemoryDevices.clear();
    m_sharedMemoryDevices.clear();
}

VmPersistentMemoryDevice HcsVirtualMachineBackend::AddPersistentMemory(const VmPersistentMemoryRequest& Request)
{
    THROW_HR_IF(E_INVALIDARG, Request.Path.empty());

    // Serialize additions so that the guest enumerates persistent memory devices in the order they
    // were added. The guest names a device after that order (/dev/pmem<index>) and callers rely on
    // the name, so the caller's wait for the device runs before the next addition begins.
    auto persistentMemoryLock = m_persistentMemoryLock.lock_exclusive();

    std::shared_ptr<GuestDeviceManager> guestDeviceManager;
    HCS_SYSTEM system{};
    HANDLE userToken{};
    VmPersistentMemoryDevice device{};
    {
        auto lock = m_lock.lock_exclusive();
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system || !m_guestDeviceManager);
        THROW_HR_IF(E_BOUNDS, m_nextDeviceId == UINT64_MAX);
        system = m_system.get();
        guestDeviceManager = m_guestDeviceManager;
        userToken = Request.UserToken ? Request.UserToken.get() : m_configuration.Description.Identity.UserToken.get();
        THROW_HR_IF_MSG(E_UNEXPECTED, !userToken, "UserToken not set for the persistent memory request");

        device.Id = {m_configuration.Description.Identity, m_nextDeviceId++};
        device.Index = m_nextPersistentMemoryIndex;
        device.EffectiveHostPath = Request.Path;
        device.ReadOnly = Request.ReadOnly;
    }

    WSL_LOG(
        "HcsAddPersistentMemoryBegin",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(device.Id.Value, "deviceId"),
        TraceLoggingValue(device.Index, "index"),
        TraceLoggingValue(Request.Path.c_str(), "path"),
        TraceLoggingValue(Request.ReadOnly, "readOnly"));

    // N.B. If this succeeds, the device would need to be removed if a later step fails. HCS does not
    //      support removing persistent memory devices, so a failure leaves the device in place; all
    //      persistent memory devices are added during VM creation, so any failure terminates the VM.
    device.GuestInstanceId = guestDeviceManager->AddVirtioPmemDevice(Request.Path.c_str(), Request.ReadOnly, userToken);

    // The device is now attached, so record it and consume its index before anything that can fail.
    // Otherwise a failure below would leave an attached but untracked device and let the next
    // addition reuse this device's guest name.
    {
        auto lock = m_lock.lock_exclusive();
        THROW_HR_IF(
            HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
            !m_system || m_system.get() != system || m_guestDeviceManager != guestDeviceManager || m_state == VmState::Stopped);
        const auto inserted = m_persistentMemoryDevices.emplace(device.Id.Value, device).second;
        WI_ASSERT(inserted);
    }

    ++m_nextPersistentMemoryIndex;

    // Give the caller a chance to observe the device in the guest while additions are still
    // serialized. This runs without m_lock because it blocks on the guest.
    if (Request.WaitForGuestDevice)
    {
        Request.WaitForGuestDevice(device.Index);
    }

    {
        auto lock = m_lock.lock_shared();
        THROW_HR_IF(
            HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
            !m_system || m_system.get() != system || m_guestDeviceManager != guestDeviceManager || m_state == VmState::Stopped);
    }

    WSL_LOG(
        "HcsAddPersistentMemoryEnd",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(device.Id.Value, "deviceId"),
        TraceLoggingValue(device.Index, "index"),
        TraceLoggingValue(device.GuestInstanceId, "instanceId"));

    return device;
}

VmGpuAttachment HcsVirtualMachineBackend::AddGpu(const VmGpuRequest& Request)
{
    const auto context = CreateExecutionContext(Context::ConfigureGpu);
    THROW_HR_IF(c_notSupported, Request.AssignmentMode != VmGpuAssignmentMode::Mirror);

    const auto disableVgpuSettingsSupported = schema::IsDisableVgpuSettingsSupported();
    VmGpuAttachment attachment{};
    attachment.AssignmentMode = Request.AssignmentMode;
    attachment.VendorExtension = validation::ValidateFeature(Request.VendorExtension, L"GPU vendor extension", true);
    attachment.GdiAccelerationDisabled =
        validation::ValidateFeature(Request.DisableGdiAcceleration, L"GPU GDI acceleration", disableVgpuSettingsSupported);
    attachment.PresentationDisabled =
        validation::ValidateFeature(Request.DisablePresentation, L"GPU presentation", disableVgpuSettingsSupported);

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), m_gpu.has_value());
    THROW_HR_IF(E_BOUNDS, m_nextDeviceId == UINT64_MAX);

    schema::AddMirroredGpu(m_system.get(), attachment.VendorExtension, attachment.GdiAccelerationDisabled, attachment.PresentationDisabled);

    attachment.Id = {m_configuration.Description.Identity, m_nextDeviceId++};
    m_gpu = attachment;

    WSL_LOG(
        "HcsAddGpu",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(attachment.Id.Value, "deviceId"),
        TraceLoggingValue(attachment.VendorExtension, "vendorExtension"),
        TraceLoggingValue(attachment.GdiAccelerationDisabled, "gdiAccelerationDisabled"),
        TraceLoggingValue(attachment.PresentationDisabled, "presentationDisabled"));

    return attachment;
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

    VmFileSystemDevice device{
        {m_configuration.Description.Identity, m_nextDeviceId},
        VmFileSystemDeviceState::Prepared,
        {},
        Request.Transport,
        wsl::windows::common::security::IsTokenElevated(userToken.get())};
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
                // A single-share device is created from the share that it serves, so device options
                // would never reach the guest.
                THROW_HR_IF_MSG(
                    E_INVALIDARG,
                    transport.Layout == VmVirtioFsLayout::SingleShare &&
                        (!transport.Options.MountOptions.empty() || !!transport.Options.UserToken),
                    "A single-share virtio-fs device takes its options from the share that it serves");

                mountOptions = FormatVirtioFsMountOptions(transport.Options.MountOptions);
                if (transport.Layout == VmVirtioFsLayout::Aggregate)
                {
                    const VirtioFsShareOptions options{.Kind = VirtiofsShareKind_Aggregate};
                    guestInstanceId = m_guestDeviceManager->AddVirtiofsDevice(
                        transport.Tag.c_str(), mountOptions.c_str(), L"", ResolveUserToken({transport.Options.UserToken}), options);
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
                THROW_HR_IF(E_INVALIDARG, transport.Port.Value == 0);
                plan9Server = transport.ServerFactory(m_configuration.Description.Identity.UserToken.get());
                THROW_HR_IF(E_UNEXPECTED, !plan9Server);

                auto runAsUser = wil::impersonate_token(m_configuration.Description.Identity.UserToken.get());
                THROW_IF_FAILED(plan9Server->Init(&m_runtimeId, transport.Port.Value));
                THROW_IF_FAILED(plan9Server->Resume());
                device.State = VmFileSystemDeviceState::Serving;
            },
            [&](const VmPlan9HostedDevice& transport) {
                THROW_HR_IF(E_INVALIDARG, transport.Port.Value == 0);
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

std::optional<VmFileSystemDevice> HcsVirtualMachineBackend::GetFileSystemDevice(const VmFileSystemDevicePredicate& Predicate) const
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

VmFileSystemDevice HcsVirtualMachineBackend::GetFileSystemDeviceStatus(VmDeviceId Device)
{
    validation::ValidateResourceId(Device, m_configuration.Description.Identity);
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system);
    const auto device = FindFileSystemDeviceLocked(Device);
    if (device->second.Backend.Plan9Server && device->second.Backend.Plan9Server->IsRunning() != S_OK)
    {
        device->second.Device.State = VmFileSystemDeviceState::Unavailable;
    }
    else if (device->second.Backend.Plan9Server)
    {
        device->second.Device.State = VmFileSystemDeviceState::Serving;
    }
    return device->second.Device;
}

HcsVirtualMachineBackend::FileSystemDeviceMap::iterator HcsVirtualMachineBackend::FindFileSystemDeviceLocked(VmDeviceId Device)
{
    const auto device = m_fileSystemDevices.find(Device.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), device == m_fileSystemDevices.end());

    return device;
}

const HcsVirtualMachineBackend::FileSystemShare* HcsVirtualMachineBackend::FindFileSystemShareLocked(
    VmDeviceId Device, const std::wstring& HostPath, const std::wstring& MountOptions, const VmFileSystemShareRequest& Request, HANDLE UserToken) const
{
    for (const auto& entry : m_fileSystemShares)
    {
        const auto& share = entry.second;
        if ((share.Share.Device.Value == Device.Value) && (share.Share.EffectiveHostPath.native() == HostPath) &&
            (share.MountOptions == MountOptions))
        {
            const auto& address = std::get<VmVirtioFsShareAddress>(share.Share.GuestAddress);
            if (!Request.Name.empty() && address.ChildName != Request.Name)
            {
                continue;
            }

            const bool equivalent = share.Share.EffectiveHostPath.native() == HostPath && share.MountOptions == MountOptions &&
                                    share.Share.ReadOnly == Request.ReadOnly &&
                                    AreSameTokens(ResolveUserToken({share.UserToken}), UserToken);
            THROW_HR_IF_MSG(
                HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
                !Request.Name.empty() && !equivalent,
                "A file system share with this name already exists with a different request");
            if (equivalent)
            {
                return &share;
            }
        }
    }

    return nullptr;
}

std::optional<VmFileSystemShare> HcsVirtualMachineBackend::FindPlan9ShareByNameLocked(
    VmDeviceId Device, const VmFileSystemShareRequest& Request, HANDLE UserToken) const
{
    const auto flags = GetPlan9ShareFlags(Request);
    if (Request.Name.empty())
    {
        return std::nullopt;
    }

    for (const auto& entry : m_fileSystemShares)
    {
        if (entry.second.Share.Device.Value != Device.Value)
        {
            continue;
        }

        const bool match = std::visit(
            Overloaded{
                [](const VmVirtioFsShareAddress&) { return false; },
                [&](const VmPlan9SocketShareAddress& address) { return address.AccessName == Request.Name; },
                [&](const VmPlan9VirtioShareAddress& address) { return address.AccessName == Request.Name; }},
            entry.second.Share.GuestAddress);
        if (match)
        {
            const auto& share = entry.second;
            THROW_HR_IF_MSG(
                HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
                share.Share.EffectiveHostPath.native() != Request.HostPath.native() || share.Plan9Flags != flags ||
                    !AreSameTokens(ResolveUserToken({share.UserToken}), UserToken),
                "A Plan 9 share with this name already exists with a different request");
            return entry.second.Share;
        }
    }

    return std::nullopt;
}

HANDLE HcsVirtualMachineBackend::ResolveUserToken(std::initializer_list<std::reference_wrapper<const wil::shared_handle>> Tokens) const
{
    for (const auto& token : Tokens)
    {
        if (token.get())
        {
            return token.get().get();
        }
    }

    HANDLE userToken = m_configuration.Description.Identity.UserToken.get();
    THROW_HR_IF_MSG(E_UNEXPECTED, !userToken, "UserToken not set");

    return userToken;
}

std::wstring HcsVirtualMachineBackend::AddPlan9ShareLocked(
    const FileSystemDevice& Device, const VmFileSystemShareRequest& Request, HANDLE UserToken, const std::wstring& HostPath) const
{
    const auto flags = GetPlan9ShareFlags(Request);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !Device.Backend.Plan9Server);

    // Allow the Plan 9 server to create NT symlinks.
    //
    // N.B. This may fail for unelevated users, however symlink creation will
    //      succeed even without this privilege if developer mode is enabled.
    wsl::windows::common::security::EnableTokenPrivilege(UserToken, SE_CREATE_SYMBOLIC_LINK_NAME);

    auto accessName = Request.Name.empty() ? GenerateShareName() : Request.Name;
    auto runAsUser = wil::impersonate_token(UserToken);
    AddPlan9SharePath(Device.Backend.Plan9Server, accessName.c_str(), HostPath.c_str(), static_cast<UINT32>(flags));

    return accessName;
}

_Requires_lock_held_(m_lock)
void HcsVirtualMachineBackend::RemovePlan9ShareLocked(const FileSystemDevice& Device, const std::wstring& AccessName, HANDLE UserToken) const
{
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !Device.Backend.Plan9Server);

    auto runAsUser = wil::impersonate_token(UserToken);
    THROW_IF_FAILED(Device.Backend.Plan9Server->RemoveShare(AccessName.c_str()));
}

VmFileSystemShare HcsVirtualMachineBackend::AddFileSystemShare(VmDeviceId Device, const VmFileSystemShareRequest& Request)
{
    validation::ValidateResourceId(Device, m_configuration.Description.Identity);
    THROW_HR_IF_MSG(E_INVALIDARG, Request.HostPath.empty(), "A host path is required");
    const auto userToken = ResolveUserToken({Request.UserToken});

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
    if (!std::holds_alternative<VmVirtioFsDevice>(device->second.Transport))
    {
        if (auto existing = FindPlan9ShareByNameLocked(Device, Request, userToken))
        {
            WSL_LOG(
                "HcsAddFileSystemShareEnd",
                TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
                TraceLoggingValue(Device.Value, "deviceId"),
                TraceLoggingValue(existing->Id.Value, "shareId"),
                TraceLoggingValue(false, "created"));

            return std::move(existing).value();
        }
    }

    // A virtio-fs share resolves to a canonical directory, while a Plan 9 share may be rooted at a
    // prefix such as '\\?' so that the guest can mount arbitrary subpaths below it.
    std::wstring hostPath;
    std::wstring mountOptions;
    std::map<std::wstring, std::wstring> effectiveMountOptions;
    auto shareUserToken = Request.UserToken;
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

                effectiveMountOptions = shareOptions;
                mountOptions = FormatVirtioFsMountOptions(shareOptions);

                if (transport.Layout == VmVirtioFsLayout::Aggregate)
                {
                    // An aggregate device reaches every child through the identity declared in its
                    // device options. A share cannot introduce a second one; callers that need
                    // another identity create another device.
                    THROW_HR_IF_MSG(
                        E_INVALIDARG,
                        !!Request.UserToken || !!options->UserToken,
                        "A share of an aggregate virtio-fs device is served under the identity of the device");
                }
                else
                {
                    THROW_HR_IF_MSG(
                        E_INVALIDARG, !Request.Name.empty(), "A single-share virtio-fs device does not accept a share name");
                    if (options->UserToken)
                    {
                        shareUserToken = options->UserToken;
                    }
                }

                // Repeating a request for the same path and options reuses the existing share so that
                // multiple guest mounts of one host directory are backed by a single virtio-fs share.
                if (const auto* existing =
                        FindFileSystemShareLocked(Device, hostPath, mountOptions, Request, ResolveUserToken({shareUserToken})))
                {
                    reused = existing->Share;
                    return;
                }

                std::optional<std::wstring> childName;
                if (transport.Layout == VmVirtioFsLayout::Aggregate)
                {
                    childName = Request.Name.empty() ? GenerateShareName() : Request.Name;
                    m_guestDeviceManager->AddVirtiofsChild(
                        device->second.Device.GuestInstanceId.value(), childName->c_str(), mountOptions.c_str(), hostPath.c_str());
                }
                else
                {
                    // A single-share device is the share, so it is created on first use and cannot be shared further.
                    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), device->second.Device.GuestInstanceId.has_value());
                    device->second.Device.GuestInstanceId = m_guestDeviceManager->AddVirtiofsDevice(
                        transport.Tag.c_str(),
                        mountOptions.c_str(),
                        hostPath.c_str(),
                        ResolveUserToken({options->UserToken, Request.UserToken}));
                }

                device->second.Device.State = VmFileSystemDeviceState::Serving;
                guestAddress = VmVirtioFsShareAddress{transport.Tag, std::move(childName)};
            },
            [&](const VmPlan9SocketDevice& transport) {
                hostPath = Request.HostPath.native();
                guestAddress = VmPlan9SocketShareAddress{transport.Port, AddPlan9ShareLocked(device->second, Request, userToken, hostPath)};
            },
            [&](const VmPlan9HostedDevice& transport) {
                hostPath = Request.HostPath.native();
                const auto flags = GetPlan9ShareFlags(Request);
                const auto accessName = Request.Name.empty() ? GenerateShareName() : Request.Name;
                schema::AddPlan9Share(
                    m_system.get(), accessName.c_str(), accessName.c_str(), hostPath.c_str(), transport.Port.Value, flags, userToken);
                guestAddress = VmPlan9SocketShareAddress{transport.Port, accessName};
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
    share.MountOptions = std::move(effectiveMountOptions);
    share.Elevated = device->second.Elevated;
    share.ReadOnly = Request.ReadOnly;

    const auto plan9Flags = std::holds_alternative<VmVirtioFsDevice>(device->second.Transport) ? schema::Plan9ShareFlags::None
                                                                                               : GetPlan9ShareFlags(Request);
    const auto inserted =
        m_fileSystemShares
            .emplace(share.Id.Value, FileSystemShare{share, std::move(mountOptions), plan9Flags, std::move(shareUserToken)})
            .second;
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

std::optional<VmFileSystemShare> HcsVirtualMachineBackend::GetFileSystemShare(const VmFileSystemSharePredicate& Predicate) const
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
            [&](const VmPlan9HostedDevice& transport) {
                const auto& guestAddress = std::get<VmPlan9SocketShareAddress>(share->second.Share.GuestAddress);
                schema::RemovePlan9Share(m_system.get(), guestAddress.AccessName.c_str(), transport.Port.Value);
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

VmSharedMemoryDevice HcsVirtualMachineBackend::AddSharedMemory(const VmSharedMemoryRequest& Request)
{
    THROW_HR_IF(
        E_INVALIDARG,
        Request.Tag.empty() || Request.Path.empty() || Request.SizeBytes == 0 || (Request.SizeBytes % c_mib) != 0 ||
            (Request.SizeBytes / c_mib) > UINT32_MAX);

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system || !m_guestDeviceManager);
    THROW_HR_IF(E_BOUNDS, m_nextDeviceId == UINT64_MAX);
    HANDLE userToken = Request.UserToken ? Request.UserToken.get() : m_configuration.Description.Identity.UserToken.get();
    THROW_HR_IF_MSG(E_UNEXPECTED, !userToken, "UserToken not set for the shared memory request");

    for (const auto& entry : m_sharedMemoryDevices)
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), wsl::shared::string::IsEqual(entry.second.Tag, Request.Tag, false));
    }

    VmSharedMemoryDevice device{};
    device.Id = {m_configuration.Description.Identity, m_nextDeviceId};
    device.Tag = Request.Tag;
    device.ObjectPath = std::format(L"WSL\\{}\\{}", m_vmIdString, Request.Path);
    device.SizeBytes = Request.SizeBytes;
    device.GuestInstanceId = m_guestDeviceManager->AddSharedMemoryDevice(
        Request.Tag.c_str(), Request.Path.c_str(), static_cast<UINT32>(Request.SizeBytes / c_mib), userToken);
    auto removeOnFailure =
        wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { m_guestDeviceManager->RemoveGuestDevice(device.GuestInstanceId); });
    const auto inserted = m_sharedMemoryDevices.emplace(device.Id.Value, device).second;
    WI_ASSERT(inserted);
    ++m_nextDeviceId;
    removeOnFailure.release();
    return device;
}

void HcsVirtualMachineBackend::ConfigureGuestDma(const VmGuestDmaRequest& Request)
{
    THROW_HR_IF(E_INVALIDARG, Request.BaseAddress == 0 || Request.SizeBytes == 0);
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system || !m_guestDeviceManager);
    m_guestDeviceManager->SetSwiotlb(Request.BaseAddress, Request.SizeBytes);
}

void HcsVirtualMachineBackend::RemoveDevice(VmDeviceId Device)
{
    validation::ValidateResourceId(Device, m_configuration.Description.Identity);
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system || !m_guestDeviceManager);

    if (const auto sharedMemory = m_sharedMemoryDevices.find(Device.Value); sharedMemory != m_sharedMemoryDevices.end())
    {
        m_guestDeviceManager->RemoveGuestDevice(sharedMemory->second.GuestInstanceId);
        m_sharedMemoryDevices.erase(sharedMemory);
        return;
    }

    if ((m_gpu && m_gpu->Id.Value == Device.Value) || m_persistentMemoryDevices.contains(Device.Value) ||
        m_networkAdapters.contains(Device.Value))
    {
        THROW_HR(c_notSupported);
    }

    const auto device = m_fileSystemDevices.find(Device.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), device == m_fileSystemDevices.end());
    const auto hasShares =
        std::ranges::any_of(m_fileSystemShares, [&](const auto& entry) { return entry.second.Device.Value == Device.Value; });
    const bool unavailablePlan9 = device->second.State == VmFileSystemDeviceState::Unavailable &&
                                  (std::holds_alternative<VmPlan9SocketDevice>(device->second.Transport) ||
                                   std::holds_alternative<VmPlan9VirtioDevice>(device->second.Transport));
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_BUSY), hasShares && !unavailablePlan9);

    if (unavailablePlan9)
    {
        std::erase_if(m_fileSystemShares, [&](const auto& entry) { return entry.second.Device.Value == Device.Value; });
    }

    std::visit(
        Overloaded{
            [&](const VmVirtioFsDevice&) {
                if (device->second.Device.GuestInstanceId)
                {
                    m_guestDeviceManager->RemoveGuestDevice(device->second.Device.GuestInstanceId.value());
                }
            },
            [&](const VmPlan9VirtioDevice& transport) {
                if (device->second.Device.GuestInstanceId)
                {
                    m_guestDeviceManager->RemoveGuestDevice(device->second.Device.GuestInstanceId.value());
                }
                m_guestDeviceManager->RemoveRemoteFileSystem(transport.FileSystemClassId, transport.Tag);
            },
            [&](const VmPlan9SocketDevice&) {
                if (device->second.Backend.Plan9Server)
                {
                    if (unavailablePlan9)
                    {
                        LOG_IF_FAILED(device->second.Backend.Plan9Server->Teardown());
                    }
                    else
                    {
                        THROW_IF_FAILED(device->second.Backend.Plan9Server->Teardown());
                    }
                }
            },
            [&](const VmPlan9HostedDevice&) {}},
        device->second.Transport);
    m_fileSystemDevices.erase(device);
}

std::map<std::uint64_t, HcsVirtualMachineBackend::NetworkAdapter>::iterator HcsVirtualMachineBackend::FindNetworkAdapterLocked(VmDeviceId Device)
{
    validation::ValidateResourceId(Device, m_configuration.Description.Identity);
    const auto adapter = m_networkAdapters.find(Device.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), adapter == m_networkAdapters.end());
    return adapter;
}

wil::com_ptr<IWslVirtioNetDevice> HcsVirtualMachineBackend::GetUserModeNatDeviceLocked(VmDeviceId Device) const
{
    validation::ValidateResourceId(Device, m_configuration.Description.Identity);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system || !m_guestDeviceManager);
    const auto adapter = m_networkAdapters.find(Device.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), adapter == m_networkAdapters.end());

    // A host endpoint network is served by the host network stack, which owns port forwarding, virtual
    // addresses and name resolution for the adapter. Only a user-mode NAT exposes them to the backend.
    THROW_HR_IF_MSG(
        c_notSupported,
        !std::holds_alternative<VmUserModeNatNetwork>(adapter->second.EffectiveConfiguration),
        "The adapter is not served by a user-mode NAT");

    auto device = m_guestDeviceManager->GetVirtioNetDevice(adapter->second.Tag.c_str());
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), !device);
    return device;
}

VmNetworkAttachment HcsVirtualMachineBackend::AddNetworkAdapter(const VmNetworkAdapterRequest& Request)
{
    const auto context = CreateExecutionContext(Context::ConfigureNetworking);

    WSL_LOG(
        "HcsAddNetworkAdapterBegin",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Request.Tag.c_str(), "tag"));

    THROW_HR_IF_MSG(E_INVALIDARG, Request.Tag.empty(), "A network adapter requires a tag");

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system || !m_guestDeviceManager);

    const auto* requestedHostEndpoint = std::get_if<VmHostEndpointNetwork>(&Request.Configuration);
    if (requestedHostEndpoint != nullptr)
    {
        THROW_HR_IF(E_INVALIDARG, IsEqualGUID(requestedHostEndpoint->EndpointId, GUID_NULL) || IsEqualGUID(requestedHostEndpoint->InstanceId, GUID_NULL));
    }

    const VmNetworkAttachment* existingAttachment = nullptr;
    for (const auto& [_, adapter] : m_networkAdapters)
    {
        bool duplicate = adapter.Tag == Request.Tag;
        if (requestedHostEndpoint != nullptr)
        {
            const auto* existingHostEndpoint = std::get_if<VmHostEndpointNetwork>(&adapter.EffectiveConfiguration);
            duplicate = duplicate || (existingHostEndpoint != nullptr &&
                                      (IsEqualGUID(existingHostEndpoint->EndpointId, requestedHostEndpoint->EndpointId) ||
                                       IsEqualGUID(existingHostEndpoint->InstanceId, requestedHostEndpoint->InstanceId)));
        }

        if (!duplicate)
        {
            continue;
        }

        THROW_HR_IF_MSG(
            E_INVALIDARG,
            existingAttachment != nullptr && existingAttachment->Id.Value != adapter.Id.Value,
            "The network adapter request identifies more than one existing adapter");
        existingAttachment = &adapter;
    }

    if (existingAttachment != nullptr)
    {
        WSL_LOG(
            "HcsAddNetworkAdapterEnd",
            TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
            TraceLoggingValue(existingAttachment->Tag.c_str(), "tag"),
            TraceLoggingValue(existingAttachment->Id.Value, "deviceId"),
            TraceLoggingValue(false, "created"));

        return *existingAttachment;
    }

    THROW_HR_IF(E_BOUNDS, m_nextDeviceId == std::numeric_limits<std::uint64_t>::max());

    VmNetworkAttachment attachment{};
    attachment.Id = VmDeviceId{m_configuration.Description.Identity, m_nextDeviceId};
    attachment.Tag = Request.Tag;
    attachment.EffectiveConfiguration = Request.Configuration;

    // A host endpoint is attached to the running compute system as a network adapter, while a
    // user-mode NAT reaches the guest through a virtio-net device served by the device host.
    std::wstring resourcePath;
    auto rollback = wil::scope_exit([&] {
        try
        {
            if (!resourcePath.empty())
            {
                const auto& configuration = std::get<VmHostEndpointNetwork>(attachment.EffectiveConfiguration);
                schema::ModifyNetworkAdapter(
                    m_system.get(),
                    resourcePath.c_str(),
                    schema::ModifyRequestType::Remove,
                    configuration.EndpointId,
                    configuration.InstanceId,
                    configuration.MacAddress);
            }
            else if (attachment.GuestInstanceId.has_value())
            {
                m_guestDeviceManager->RemoveGuestDevice(attachment.GuestInstanceId.value());
            }
        }
        CATCH_LOG();
    });

    std::visit(
        Overloaded{
            [&](const VmHostEndpointNetwork& configuration) {
                resourcePath = wsl::core::networking::c_networkAdapterPrefix +
                               wsl::shared::string::GuidToString<wchar_t>(configuration.InstanceId);
                schema::ModifyNetworkAdapter(
                    m_system.get(),
                    resourcePath.c_str(),
                    schema::ModifyRequestType::Add,
                    configuration.EndpointId,
                    configuration.InstanceId,
                    configuration.MacAddress,
                    true);
                attachment.GuestInstanceId = configuration.InstanceId;
            },
            [&](const VmUserModeNatNetwork& configuration) {
                attachment.GuestInstanceId = m_guestDeviceManager->AddVirtioNetDevice(
                    Request.Tag.c_str(),
                    configuration.Configuration,
                    configuration.Nameservers,
                    m_configuration.Description.Identity.UserToken.get());
            }},
        Request.Configuration);

    const auto inserted = m_networkAdapters.emplace(attachment.Id.Value, NetworkAdapter{attachment, resourcePath}).second;
    WI_ASSERT(inserted);
    ++m_nextDeviceId;
    m_configuration.Description.NetworkAdapters[Request.Tag] = attachment;
    rollback.release();

    WSL_LOG(
        "HcsAddNetworkAdapterEnd",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Request.Tag.c_str(), "tag"),
        TraceLoggingValue(attachment.Id.Value, "deviceId"),
        TraceLoggingValue(true, "created"));

    return attachment;
}

void HcsVirtualMachineBackend::UpdateNetworkAdapter(VmDeviceId Device, const VmNetworkConfiguration& Configuration)
{
    const auto context = CreateExecutionContext(Context::ConfigureNetworking);

    WSL_LOG(
        "HcsUpdateNetworkAdapterBegin",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"));

    // A user-mode NAT can be reconfigured in place; a host endpoint is reconfigured on the host and
    // has to be removed and added again to change the endpoint the adapter is bound to.
    const auto* configuration = std::get_if<VmUserModeNatNetwork>(&Configuration);
    THROW_HR_IF_MSG(c_notSupported, !configuration, "A host endpoint adapter cannot be updated in place");

    auto lock = m_lock.lock_exclusive();
    auto adapter = FindNetworkAdapterLocked(Device);
    auto device = GetUserModeNatDeviceLocked(Device);
    IpAddress emptyNameserver{};
    auto* nameservers = configuration->Nameservers.empty() ? &emptyNameserver : const_cast<IpAddress*>(configuration->Nameservers.data());
    THROW_IF_FAILED(device->Update(
        const_cast<WslVirtioNetConfig*>(&configuration->Configuration), gsl::narrow_cast<UINT32>(configuration->Nameservers.size()), nameservers));

    adapter->second.EffectiveConfiguration = Configuration;
    m_configuration.Description.NetworkAdapters[adapter->second.Tag] = adapter->second;

    WSL_LOG(
        "HcsUpdateNetworkAdapterEnd",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"));
}

void HcsVirtualMachineBackend::RemoveNetworkAdapterLocked(std::map<std::uint64_t, NetworkAdapter>::iterator Adapter)
{
    const auto& attachment = Adapter->second;
    std::visit(
        Overloaded{
            [&](const VmHostEndpointNetwork& configuration) {
                schema::ModifyNetworkAdapter(
                    m_system.get(),
                    Adapter->second.ResourcePath.c_str(),
                    schema::ModifyRequestType::Remove,
                    configuration.EndpointId,
                    configuration.InstanceId,
                    configuration.MacAddress);
            },
            [&](const VmUserModeNatNetwork&) {
                // Tearing down the relay is best effort: the device is removed either way, and a
                // device host that already exited has released the ports with it.
                if (auto device = m_guestDeviceManager->GetVirtioNetDevice(attachment.Tag.c_str()))
                {
                    LOG_IF_FAILED(device->Teardown());
                }

                if (attachment.GuestInstanceId.has_value())
                {
                    m_guestDeviceManager->RemoveGuestDevice(attachment.GuestInstanceId.value());
                }
            }},
        attachment.EffectiveConfiguration);

    // The bindings were served by the adapter that is going away, so they no longer exist.
    std::erase_if(m_portBindings, [&](const auto& entry) { return entry.second.Binding.Device.Value == attachment.Id.Value; });
    m_configuration.Description.NetworkAdapters.erase(attachment.Tag);
    m_networkAdapters.erase(Adapter);
}

void HcsVirtualMachineBackend::RemoveNetworkAdapter(VmDeviceId Device)
{
    const auto context = CreateExecutionContext(Context::ConfigureNetworking);

    WSL_LOG(
        "HcsRemoveNetworkAdapterBegin",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"));

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system || !m_guestDeviceManager);
    RemoveNetworkAdapterLocked(FindNetworkAdapterLocked(Device));

    WSL_LOG(
        "HcsRemoveNetworkAdapterEnd",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"));
}

void HcsVirtualMachineBackend::CloseNetworkAdaptersLocked() noexcept
{
    WSL_LOG(
        "HcsCloseNetworkAdapters",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(m_networkAdapters.size(), "adapterCount"));

    // The compute system is going away with its adapters, so only the device host relays, which
    // outlive it, have to be torn down.
    for (const auto& entry : m_networkAdapters)
    {
        if (std::holds_alternative<VmUserModeNatNetwork>(entry.second.EffectiveConfiguration) && m_guestDeviceManager)
        {
            try
            {
                if (auto device = m_guestDeviceManager->GetVirtioNetDevice(entry.second.Tag.c_str()))
                {
                    LOG_IF_FAILED(device->Teardown());
                }
            }
            CATCH_LOG();
        }
    }

    m_portBindings.clear();
    m_networkAdapters.clear();
}

VmPortBinding HcsVirtualMachineBackend::BindPort(VmDeviceId Device, const VmPortBindingRequest& Request)
{
    const auto context = CreateExecutionContext(Context::ConfigureNetworking);

    THROW_HR_IF(E_INVALIDARG, Request.GuestPort == 0);
    THROW_HR_IF(E_INVALIDARG, Request.Protocol != TransportProtocol_Tcp && Request.Protocol != TransportProtocol_Udp);
    THROW_HR_IF_MSG(c_notSupported, Request.ListenScopeId != 0, "HCS user-mode NAT does not support scoped listen addresses");
    const auto& listenAddress = Request.ListenAddress;

    WSL_LOG(
        "HcsBindPortBegin",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(
            wsl::windows::common::string::IpAddressToWstring(Request.ListenAddress, Request.ListenScopeId).c_str(),
            "listenAddress"),
        TraceLoggingValue(Request.GuestPort, "guestPort"));

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(E_BOUNDS, m_nextPortBindingId == std::numeric_limits<std::uint64_t>::max());
    auto adapter = FindNetworkAdapterLocked(Device);
    auto device = GetUserModeNatDeviceLocked(Device);

    auto listen = listenAddress;
    UINT16 hostPort = 0;
    THROW_IF_FAILED(device->BindPort(Request.Protocol, &listen, Request.HostPort, Request.GuestPort, &hostPort));

    VmPortBinding binding{};
    binding.Id = VmPortBindingId{m_configuration.Description.Identity, m_nextPortBindingId};
    binding.Device = Device;
    binding.Protocol = Request.Protocol;
    binding.EffectiveListenAddress = Request.ListenAddress;
    binding.EffectiveListenScopeId = Request.ListenScopeId;
    binding.EffectiveHostPort = hostPort;
    binding.GuestPort = Request.GuestPort;
    // A zero listen port asks the relay to allocate one, so report the port it actually listens on.

    const auto inserted = m_portBindings.emplace(binding.Id.Value, PortBinding{binding, adapter->second.Tag}).second;
    WI_ASSERT(inserted);
    ++m_nextPortBindingId;

    WSL_LOG(
        "HcsBindPortEnd",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(binding.Id.Value, "bindingId"),
        TraceLoggingValue(
            wsl::windows::common::string::IpAddressToWstring(binding.EffectiveListenAddress, binding.EffectiveListenScopeId).c_str(),
            "listenAddress"),
        TraceLoggingValue(hostPort, "hostPort"));

    return binding;
}

void HcsVirtualMachineBackend::UnbindPort(VmPortBindingId Binding)
{
    const auto context = CreateExecutionContext(Context::ConfigureNetworking);

    WSL_LOG(
        "HcsUnbindPortBegin",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Binding.Value, "bindingId"));

    validation::ValidateResourceId(Binding, m_configuration.Description.Identity);

    auto lock = m_lock.lock_exclusive();
    const auto binding = m_portBindings.find(Binding.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), binding == m_portBindings.end());
    auto device = GetUserModeNatDeviceLocked(binding->second.Binding.Device);

    // The relay tracks a binding by the guest port it forwards to, per protocol and address family.
    THROW_IF_FAILED(device->UnbindPort(
        binding->second.Binding.Protocol, binding->second.Binding.EffectiveListenAddress.family, binding->second.Binding.GuestPort));

    m_portBindings.erase(binding);

    WSL_LOG(
        "HcsUnbindPortEnd",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Binding.Value, "bindingId"));
}

IpAddress HcsVirtualMachineBackend::CreateVirtualAddress(VmDeviceId Device, const IpAddress& Destination)
{
    const auto context = CreateExecutionContext(Context::ConfigureNetworking);

    WSL_LOG(
        "HcsCreateVirtualAddressBegin",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(wsl::windows::common::string::IpAddressToWstring(Destination).c_str(), "destination"));

    auto lock = m_lock.lock_exclusive();
    auto device = GetUserModeNatDeviceLocked(Device);

    auto destination = Destination;
    IpAddress virtualAddress{};
    THROW_IF_FAILED(device->CreateVirtualAddress(&destination, &virtualAddress));

    WSL_LOG(
        "HcsCreateVirtualAddressEnd",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(wsl::windows::common::string::IpAddressToWstring(virtualAddress).c_str(), "virtualAddress"));

    return virtualAddress;
}

void HcsVirtualMachineBackend::CreateDnsRecord(VmDeviceId Device, const VmDnsRecord& Record)
{
    const auto context = CreateExecutionContext(Context::ConfigureNetworking);

    THROW_HR_IF_MSG(E_INVALIDARG, Record.Name.empty(), "A DNS record requires a name");
    THROW_HR_IF_MSG(c_notSupported, Record.Type != DnsRecordType_A, "Only A records are supported");
    THROW_HR_IF(E_INVALIDARG, Record.Address.family != IpAddressFamily_V4);
    const auto address = wsl::windows::common::string::IpAddressToString(Record.Address);

    WSL_LOG(
        "HcsCreateDnsRecordBegin",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(Record.Name.c_str(), "name"),
        TraceLoggingValue(address.c_str(), "address"));

    auto lock = m_lock.lock_exclusive();
    auto device = GetUserModeNatDeviceLocked(Device);
    THROW_IF_FAILED(device->CreateDNSRecord(Record.Type, Record.Name.c_str(), address.c_str()));

    WSL_LOG(
        "HcsCreateDnsRecordEnd",
        TraceLoggingValue(m_configuration.Description.Identity.VmId, "vmId"),
        TraceLoggingValue(Device.Value, "deviceId"),
        TraceLoggingValue(Record.Name.c_str(), "name"));
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
        backend->OnExit(Event);
    }
}
CATCH_LOG();

void HcsVirtualMachineBackend::OnCrash(PCWSTR Details)
{
    auto lock = m_crashInformationLock.lock_exclusive();
    if (m_vmCrashLogCaptured && m_vmSavedStateCaptured)
    {
        return;
    }

    auto signalCrash = wil::scope_exit([&] { m_vmCrashEvent.SetEvent(); });
    WSL_LOG("GuestCrash", TraceLoggingValue(Details, "Data"));
    const auto crashInformation = wsl::shared::FromJson<wsl::windows::common::hcs::CrashReport>(Details);

    if (!m_vmSavedStateCaptured && m_crashCapture && m_crashCapture->SavedStateFolder && m_vmSavedStateFile &&
        crashInformation.GuestCrashSaveInfo && crashInformation.GuestCrashSaveInfo->SaveStateFile)
    {
        schema::EnforceVmSavedStateFileLimit(
            m_crashCapture->SavedStateFolder.value(),
            static_cast<size_t>(m_crashCapture->MaxSavedStateCount) + 1,
            m_configuration.Description.Identity.UserToken.get());
        m_vmSavedStateCaptured = true;
    }

    if (!m_vmCrashLogCaptured && m_crashCapture && !m_crashCapture->Path.empty() && !crashInformation.CrashLog.empty())
    {
        m_vmCrashLogFile = wsl::windows::common::hcs::WriteVmCrashLog(
            m_crashCapture->Path,
            m_crashCapture->MaxCrashLogCount,
            m_configuration.Description.Identity.VmId,
            m_configuration.Description.Identity.UserToken.get(),
            crashInformation.CrashLog);
        m_vmCrashLogCaptured = true;
    }
}

void HcsVirtualMachineBackend::OnExit(const HCS_EVENT* Event)
{
    VmTerminationInformation information;
    if (Event->EventData != nullptr)
    {
        information.Details = Event->EventData;
        if (Event->Type == HcsEventSystemExited)
        {
            const auto exitStatus = wsl::shared::FromJson<schema::SystemExitStatus>(Event->EventData);
            if (exitStatus.ExitType.has_value())
            {
                switch (exitStatus.ExitType.value())
                {
                case schema::NotificationType::ForcedExit:
                case schema::NotificationType::GracefulExit:
                    information.Reason = VmTerminationReason::Shutdown;
                    break;
                case schema::NotificationType::UnexpectedExit:
                    information.Reason = VmTerminationReason::Crashed;
                    break;
                default:
                    break;
                }
            }
        }
    }

    // Closing the system drains callbacks before their event and context are destroyed.
    // An exit without a prior termination request must cancel pending operations.
    {
        auto lock = m_lock.lock_exclusive();
        m_state = VmState::Stopped;
    }

    {
        auto terminationInformationLock = m_terminationInformationLock.lock_exclusive();
        m_terminationInformation = std::move(information);
    }

    m_exitEvent.SetEvent();

    // Callers blocked on the guest must not wait forever when the VM exits on its own.
    CancelGuestListeners();

    if (!m_terminatingEvent.is_signaled())
    {
        WSL_LOG("AbnormalVmExit", TraceLoggingValue(Event->EventData, "Details"));
        m_terminatingEvent.SetEvent();
    }

    NotifyTerminated(m_configuration.Description.Identity);
}