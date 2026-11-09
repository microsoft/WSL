/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VirtualMachineBackend.cpp

Abstract:

    Provides factory functions for creating and querying virtual machine backends.

--*/

#include "precomp.h"
#include "HcsVirtualMachineBackend.h"
#include "string.hpp"
#include "IVirtualMachineBackend.h"

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

VmGuestListener::VmGuestListener(wil::unique_socket ListenerSocket) noexcept : Socket(std::move(ListenerSocket))
{
}

std::unique_ptr<IVirtualMachineBackend> CreateVirtualMachineBackend(BackendKind Kind, const VmCreateRequest& Request)
{
    switch (Kind)
    {
    case BackendKind::OpenVmm:
        THROW_HR(wsl::windows::common::vm::c_notSupported);

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
        THROW_HR(wsl::windows::common::vm::c_notSupported);

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