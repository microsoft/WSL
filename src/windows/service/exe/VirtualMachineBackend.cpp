/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VirtualMachineBackend.cpp

Abstract:

    Provides factory functions for creating and querying virtual machine backends.

--*/

#include "precomp.h"
#include "hvsocket.hpp"
#include "socket.hpp"
#include "IVirtualMachineBackend.h"
#include "OpenVmmVirtualMachineBackend.h"

namespace {

constexpr HRESULT c_notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

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

void wsl::windows::common::vm::validation::ValidateUnsupportedSelection(VmSelectionPolicy Policy)
{
    THROW_HR_IF(E_INVALIDARG, Policy != VmSelectionPolicy::Required && Policy != VmSelectionPolicy::Preferred);
    THROW_HR_IF(c_notSupported, Policy == VmSelectionPolicy::Required);
}

void wsl::windows::common::vm::validation::ValidatePath(const std::filesystem::path& Path, PCWSTR Backend)
{
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        Path.empty() || !Path.is_absolute() || Path.native().find(L'\0') != std::wstring::npos,
        "%ls requires an absolute, nonempty host path",
        Backend);
}

const VmVirtualDiskSource& wsl::windows::common::vm::validation::ValidateDiskRequest(const VmDiskRequest& Request, UINT32 MaximumDisks)
{
    const auto* source = std::get_if<VmVirtualDiskSource>(&Request.Source);
    THROW_HR_IF(c_notSupported, source == nullptr);
    ValidatePath(source->Path, L"OpenVMM");
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

    if (Request.Placement)
    {
        THROW_HR_IF(c_notSupported, Request.Placement->Address.Controller != 0 || Request.Placement->Address.Lun >= MaximumDisks);
    }

    return *source;
}

void wsl::windows::common::vm::validation::ValidateConsolePath(const std::filesystem::path& Path, PCWSTR Backend, HRESULT Error, bool RequireName)
{
    ValidatePath(Path, Backend);
    THROW_HR_IF_MSG(
        Error,
        !Path.native().starts_with(L"\\\\.\\pipe\\") || (RequireName && Path.filename().empty()),
        "%ls consoles require a caller-provided named pipe",
        Backend);
}

void wsl::windows::common::vm::validation::ValidateName(std::wstring_view Name, PCWSTR Description)
{
    THROW_HR_IF_MSG(
        E_INVALIDARG, Name.empty() || Name.find(L'\0') != std::wstring_view::npos, "%ls must be nonempty and cannot contain NUL", Description);
}

void wsl::windows::common::vm::validation::ValidateResourceId(UINT64 Value, const GUID& VmId, const VmInstanceId& Owner)
{
    THROW_HR_IF(E_INVALIDARG, Value == 0 || !IsEqualGUID(VmId, Owner.VmId));
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

    std::thread([callback = std::move(Callback), vmId]() {
        try
        {
            wsl::windows::common::wslutil::SetThreadDescription(L"VmTerminationCallback");
            callback(vmId);
        }
        CATCH_LOG();
    }).detach();
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

    const VmGuestListener listener{{Identity, m_nextListenerId}, Port};
    auto state = ConfigureGuestListener(listener);
    THROW_HR_IF(E_UNEXPECTED, !state);
    THROW_HR_IF(E_UNEXPECTED, state->Listener.Id.Value != listener.Id.Value || state->Listener.Port.Value != listener.Port.Value);

    const auto inserted = m_guestListeners.emplace(listener.Id.Value, std::move(state)).second;
    WI_ASSERT(inserted);
    ++m_nextListenerId;
    return listener;
}

wil::unique_socket IVirtualMachineBackend::AcceptGuestListenerConnection(VmListenerId Listener, const VmInstanceId& Identity) const
{
    WSL_LOG(
        "OpenVmmAcceptGuestConnectionBegin",
        TraceLoggingValue(Identity.VmId, "vmId"),
        TraceLoggingValue(Listener.Value, "listenerId"));
    THROW_HR_IF(E_INVALIDARG, Listener.Value == 0 || !IsEqualGUID(Listener.Owner.VmId, Identity.VmId));
    std::shared_ptr<VmGuestListenerState> listener;
    {
        auto lock = m_lock.lock_shared();
        const auto entry = m_guestListeners.find(Listener.Value);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), entry == m_guestListeners.end());
        listener = entry->second;
    }

    auto socket = wsl::windows::common::socket::CancellableAccept(listener->Socket.get(), INFINITE, listener->CancellationEvent.get());
    WSL_LOG(
        "OpenVmmAcceptGuestConnectionEnd",
        TraceLoggingValue(Identity.VmId, "vmId"),
        TraceLoggingValue(Listener.Value, "listenerId"),
        TraceLoggingValue(listener->Listener.Port.Value, "port"),
        TraceLoggingHResult(socket ? S_OK : E_ABORT, "result"));
    THROW_HR_IF(E_ABORT, !socket);
    return std::move(*socket);
}

std::shared_ptr<VmGuestListenerState> IVirtualMachineBackend::RemoveGuestListenerLocked(VmListenerId Listener, const VmInstanceId& Identity)
{
    WSL_LOG(
        "OpenVmmCloseGuestListener", TraceLoggingValue(Identity.VmId, "vmId"), TraceLoggingValue(Listener.Value, "listenerId"));
    THROW_HR_IF(E_INVALIDARG, Listener.Value == 0 || !IsEqualGUID(Listener.Owner.VmId, Identity.VmId));
    const auto entry = m_guestListeners.find(Listener.Value);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), entry == m_guestListeners.end());
    THROW_IF_WIN32_BOOL_FALSE(SetEvent(entry->second->CancellationEvent.get()));
    auto result = std::move(entry->second);
    m_guestListeners.erase(entry);
    return result;
}

void IVirtualMachineBackend::CloseGuestListenersLocked(const VmInstanceId& Identity) noexcept
{
    WSL_LOG(
        "OpenVmmCloseGuestListeners",
        TraceLoggingValue(Identity.VmId, "vmId"),
        TraceLoggingValue(m_guestListeners.size(), "listenerCount"));
    for (const auto& entry : m_guestListeners)
    {
        LOG_IF_WIN32_BOOL_FALSE(SetEvent(entry.second->CancellationEvent.get()));
    }

    m_guestListeners.clear();
}

std::shared_ptr<VmGuestListenerState> IVirtualMachineBackend::ConfigureGuestListener(const VmGuestListener& Listener)
{
    auto state = std::make_shared<VmGuestListenerState>();
    state->Listener = Listener;
    state->Socket = wsl::windows::common::hvsocket::Listen(Listener.Id.Owner.VmId, Listener.Port.Value);
    return state;
}

std::unique_ptr<IVirtualMachineBackend> CreateVirtualMachineBackend(BackendKind Kind, const VmCreateRequest& Request)
{
    switch (Kind)
    {
    case BackendKind::OpenVmm:
        return OpenVmmVirtualMachineBackend::Create(Request);

    case BackendKind::Hcs:
        THROW_HR(E_NOTIMPL);
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
        THROW_HR(E_NOTIMPL);
    }

    THROW_HR(E_INVALIDARG);
}