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
constexpr HRESULT c_notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

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
    memory.SizeBytes = Request.SizeBytes;
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

    m_restrictedToken = wsl::windows::common::security::CreateRestrictedToken(Request.Identity.UserToken.get());

    VmConfiguration configuration{};
    configuration.Settings.Owner = Request.Owner;
    configuration.Settings.ShouldTerminateOnLastHandleClosed = true;
    auto& description = configuration.Description;
    description.Identity = Request.Identity;
    description.Backend = BackendKind::Hcs;
    description.Processor = ConfigureProcessor(Request.Processor, configuration.Settings.VirtualMachine.ComputeTopology.Processor);
    description.Memory = ConfigureMemory(Request.Memory, configuration.Settings.VirtualMachine.ComputeTopology.Memory);
    description.Boot = ConfigureBoot(Request.Boot, configuration.Settings.VirtualMachine.Chipset);
    signalEarlyTermination.release();
    return configuration;
}

HcsVirtualMachineBackend::HcsVirtualMachineBackend() = default;

HcsVirtualMachineBackend::~HcsVirtualMachineBackend() noexcept
{
    {
        auto lock = m_lock.lock_exclusive();
        m_system.reset();
    }

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
    auto lock = m_lock.lock_exclusive();
    m_system = schema::CreateComputeSystem(id.c_str(), settings.c_str());
    schema::RegisterCallback(m_system.get(), OnSystemEvent, this);
}

VmPlatformCapabilities HcsVirtualMachineBackend::GetCapabilities() const
{
    return {.Backend = BackendKind::Hcs};
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
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_system);
    schema::TerminateComputeSystem(m_system.get());
    m_system.reset();
    CloseGuestListenersLocked(m_configuration.Description.Identity);
    // A system terminated before Start may not send an exit notification.
    m_terminatingEvent.SetEvent();
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

VmDiskAttachment HcsVirtualMachineBackend::AttachDisk(const VmDiskRequest&)
{
    THROW_HR(c_notSupported);
}

void HcsVirtualMachineBackend::DetachDisk(VmDiskId)
{
    THROW_HR(c_notSupported);
}

VmFileSystemDevice HcsVirtualMachineBackend::CreateFileSystemDevice(const VmFileSystemDeviceRequest&)
{
    THROW_HR(c_notSupported);
}

VmFileSystemShare HcsVirtualMachineBackend::AddFileSystemShare(VmDeviceId, const VmFileSystemShareRequest&)
{
    THROW_HR(c_notSupported);
}

void HcsVirtualMachineBackend::RemoveFileSystemShare(VmShareId)
{
    THROW_HR(c_notSupported);
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