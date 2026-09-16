// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "IVirtualMachineBackend.h"
#include "OpenVmmVirtualMachineBackend.h"

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