// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "Common.h"
#include "IVirtualMachineBackend.h"

namespace VirtualMachineBackendTestHelpers {

constexpr UINT64 c_mib = 1024 * 1024;

inline VmCreateRequest CreateRequest()
{
    VmCreateRequest request;
    THROW_IF_FAILED(CoCreateGuid(&request.Identity.VmId));
    request.Identity.UserToken = wil::shared_handle{GetNonElevatedToken(TokenImpersonation).release()};
    request.Owner = L"WSL tests";
    request.Processor.Count = 2;
    request.Memory.SizeBytes = 512 * c_mib;
    request.Boot.KernelPath = L"C:\\images\\kernel";
    request.Boot.InitrdPath = L"C:\\images\\initrd";
    return request;
}

inline VmCreateRequest CreateRunnableRequest()
{
    auto request = CreateRequest();
    const auto basePath = wsl::windows::common::wslutil::GetBasePath();
    request.Boot.KernelPath = basePath / L"kernel";
    request.Boot.InitrdPath = basePath / LXSS_VM_MODE_INITRD_NAME;
    request.Boot.KernelCommandLine = L"panic=-1";
    return request;
}

template <typename Callback>
HRESULT OperationResult(Callback&& Operation)
{
    return wil::ResultFromException(std::forward<Callback>(Operation));
}

inline void VerifyBootsAndTerminates(std::unique_ptr<IVirtualMachineBackend> Backend)
{
    auto terminationEvent = Backend->GetTerminationEvent();
    Backend->Start();

    const auto runningResult = WaitForSingleObject(terminationEvent.get(), 100);
    VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), runningResult);
    if (runningResult == WAIT_TIMEOUT)
    {
        Backend->Terminate();
    }

    VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(terminationEvent.get(), 30 * 1000));
}

} // namespace VirtualMachineBackendTestHelpers
