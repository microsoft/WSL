/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VirtualMachineBackend.cpp

Abstract:

    Provides factory functions for creating and querying virtual machine backends.

--*/

#include "precomp.h"
#include "HcsVirtualMachineBackend.h"
#include "socket.hpp"
#include "string.hpp"
#include "IVirtualMachineBackend.h"
#include "OpenVmmVirtualMachineBackend.h"

using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;

std::unique_ptr<ExecutionContext> wsl::windows::common::vm::CreateExecutionContext(Context Context)
{
    const auto* current = ExecutionContext::Current();
    if (current != nullptr && current->CurrentContext() >= static_cast<ULONGLONG>(Context))
    {
        return {};
    }

    return std::make_unique<ExecutionContext>(Context);
}

wsl::shared::string::MacAddress VmUserModeNatNetwork::ClientMacAddress() const
{
    wsl::shared::string::MacAddress address{};
    std::copy(std::begin(Configuration.clientMac.bytes), std::end(Configuration.clientMac.bytes), address.begin());
    return address;
}

bool wsl::windows::common::vm::validation::ValidateFeature(VmFeatureRequest Request, PCWSTR Setting, bool Supported)
{
    switch (Request)
    {
    case VmFeatureRequest::Disabled:
        return false;
    case VmFeatureRequest::Preferred:
        return Supported;
    case VmFeatureRequest::Required:
        THROW_HR_IF_MSG(c_notSupported, !Supported, "The backend does not support the required %ls setting", Setting);
        return true;
    }

    THROW_HR(E_INVALIDARG);
}

void IVirtualMachineBackend::RegisterTerminationCallback(TerminationCallback Callback)
{
    if (!Callback)
    {
        return;
    }

    WSL_LOG("VirtualMachineBackendRegisterTerminationCallback");
    GUID vmId{};
    {
        auto lock = m_terminationCallbackLock.lock_exclusive();
        THROW_HR_IF(E_INVALIDARG, m_terminationCallback);
        if (!m_terminated)
        {
            m_terminationCallback = std::move(Callback);
            return;
        }

        vmId = m_terminatedVmId;
    }

    // If the termination callback was registered after the VM has already terminated,
    // invoke it immediately with the terminated VM's ID.
    try
    {
        Callback(vmId);
    }
    CATCH_LOG();
}

void IVirtualMachineBackend::NotifyTerminated(const VmInstanceId& Identity) noexcept
{
    TerminationCallback callback;
    {
        auto lock = m_terminationCallbackLock.lock_exclusive();
        if (m_terminated)
        {
            return;
        }

        m_terminated = true;
        m_terminatedVmId = Identity.VmId;
        callback = std::move(m_terminationCallback);
    }

    if (callback)
    {
        try
        {
            callback(Identity.VmId);
        }
        CATCH_LOG();
    }
}

VmGuestListener IVirtualMachineBackend::RegisterGuestListenerLocked(const VmInstanceId& Identity, GuestServicePort Port)
{
    THROW_HR_IF(E_BOUNDS, m_nextListenerId == UINT64_MAX);
    for (const auto& entry : m_guestListeners)
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), entry.second->Listener.Port.Value == Port.Value);
    }

    VmGuestListener listener{{Identity, m_nextListenerId}, Port};
    auto state = ConfigureGuestListener(listener);
    THROW_HR_IF(E_UNEXPECTED, !state);
    THROW_HR_IF(E_UNEXPECTED, state->Listener.Id.Value != listener.Id.Value || state->Listener.Port.Value != listener.Port.Value);

    const auto inserted = m_guestListeners.emplace(listener.Id.Value, std::move(state)).second;
    WI_ASSERT(inserted);
    listener.State = m_guestListeners.at(listener.Id.Value);
    ++m_nextListenerId;
    return listener;
}

wil::unique_socket VmGuestListener::Accept(DWORD Timeout, const std::source_location& Location) const
{
    THROW_HR_IF(E_INVALIDARG, Id.Value == 0 || !State);
    WSL_LOG("VmAcceptGuestConnectionBegin", TraceLoggingValue(Id.Owner.VmId, "vmId"), TraceLoggingValue(Id.Value, "listenerId"));

    auto socket = wsl::windows::common::socket::CancellableAccept(State->Socket.get(), Timeout, State->CancellationEvent.get(), Location);
    WSL_LOG(
        "VmAcceptGuestConnectionEnd",
        TraceLoggingValue(Id.Owner.VmId, "vmId"),
        TraceLoggingValue(Id.Value, "listenerId"),
        TraceLoggingValue(Port.Value, "port"),
        TraceLoggingHResult(socket ? S_OK : E_ABORT, "result"));
    THROW_HR_IF(E_ABORT, !socket);
    return std::move(*socket);
}

SOCKET VmGuestListener::Socket() const
{
    THROW_HR_IF(E_INVALIDARG, Id.Value == 0 || !State);
    return State->Socket.get();
}

std::shared_ptr<VmGuestListenerState> IVirtualMachineBackend::RemoveGuestListenerLocked(VmListenerId Listener, const VmInstanceId& Identity)
{
    WSL_LOG("VmCloseGuestListener", TraceLoggingValue(Identity.VmId, "vmId"), TraceLoggingValue(Listener.Value, "listenerId"));
    THROW_HR_IF(E_INVALIDARG, Listener.Value == 0 || !IsEqualGUID(Listener.Owner.VmId, Identity.VmId));
    const auto entry = m_guestListeners.find(Listener.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), entry == m_guestListeners.end());
    THROW_IF_WIN32_BOOL_FALSE(SetEvent(entry->second->CancellationEvent.get()));
    auto result = std::move(entry->second);
    m_guestListeners.erase(entry);
    return result;
}

void IVirtualMachineBackend::CancelGuestListeners() noexcept
{
    auto lock = m_lock.lock_shared();
    for (const auto& entry : m_guestListeners)
    {
        LOG_IF_WIN32_BOOL_FALSE(SetEvent(entry.second->CancellationEvent.get()));
    }
}

void IVirtualMachineBackend::CloseGuestListenersLocked(const VmInstanceId& Identity) noexcept
{
    WSL_LOG(
        "VmCloseGuestListeners",
        TraceLoggingValue(Identity.VmId, "vmId"),
        TraceLoggingValue(m_guestListeners.size(), "listenerCount"));
    for (const auto& entry : m_guestListeners)
    {
        LOG_IF_WIN32_BOOL_FALSE(SetEvent(entry.second->CancellationEvent.get()));
    }

    m_guestListeners.clear();
}

std::unique_ptr<IVirtualMachineBackend> CreateVirtualMachineBackend(BackendKind Kind, const VmCreateRequest& Request)
{
    switch (Kind)
    {
    case BackendKind::OpenVmm:
        return OpenVmmVirtualMachineBackend::Create(Request);

    case BackendKind::Hcs:
        return HcsVirtualMachineBackend::Create(Request);
    }

    THROW_HR(E_INVALIDARG);
}

VmPlatformCapabilities QueryVirtualMachineBackendCapabilities(BackendKind Kind)
{
    switch (Kind)
    {
    case BackendKind::OpenVmm:
        return OpenVmmVirtualMachineBackend::QueryCapabilities();

    case BackendKind::Hcs:
        return HcsVirtualMachineBackend::QueryCapabilities();
    }

    THROW_HR(E_INVALIDARG);
}

void wsl::windows::common::vm::validation::ValidateDiskPlacement(const VmDiskRequest& Request)
{
    if (Request.Placement)
    {
        THROW_HR_IF(c_notSupported, Request.Placement->Address.Controller != 0 || Request.Placement->Address.Lun >= c_maximumDisks);
    }
}

const VmVirtualDiskSource& wsl::windows::common::vm::validation::ValidateDiskRequest(const VmDiskRequest& Request)
{
    const auto* source = std::get_if<VmVirtualDiskSource>(&Request.Source);
    THROW_HR_IF(c_notSupported, source == nullptr);
    switch (source->Format)
    {
    case VmDiskFormat::Vhd:
        THROW_HR_IF(E_INVALIDARG, _wcsicmp(source->Path.extension().c_str(), L".vhd") != 0);
        break;
    case VmDiskFormat::Vhdx:
        THROW_HR_IF(E_INVALIDARG, _wcsicmp(source->Path.extension().c_str(), L".vhdx") != 0);
        break;
    default:
        THROW_HR(E_INVALIDARG);
    }

    ValidateDiskPlacement(Request);

    return *source;
}

const std::wstring& wsl::windows::common::vm::validation::ValidateDiskSource(const VmDiskRequest& Request)
{
    const auto* physicalSource = std::get_if<VmPhysicalDiskSource>(&Request.Source);
    if (physicalSource == nullptr)
    {
        return ValidateDiskRequest(Request).Path.native();
    }

    THROW_HR_IF_MSG(
        E_INVALIDARG,
        physicalSource->DevicePath.empty() || physicalSource->DevicePath.find(L'\0') != std::wstring::npos,
        "A nonempty device path is required");

    ValidateDiskPlacement(Request);

    return physicalSource->DevicePath;
}