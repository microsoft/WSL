// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "Common.h"
#include "IVirtualMachineBackend.h"
#include "SocketChannel.h"

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
    request.Boot.KernelCommandLine = TEXT(WSL_ROOT_INIT_ENV) L"=1 panic=-1";
    return request;
}

inline std::pair<wsl::shared::SocketChannel, wil::unique_socket> StartGuest(IVirtualMachineBackend& Backend)
{
    const auto listener = Backend.CreateGuestListener({LX_INIT_UTILITY_VM_INIT_PORT});
    auto closeListener = wil::scope_exit([&] { Backend.CloseGuestListener(listener.Id); });
    Backend.Start();

    // Keep both channels open so mini_init can wait for configuration instead of exiting.
    wsl::shared::SocketChannel channel{listener.Accept(30 * 1000), "BackendTest"};
    channel.ReceiveMessage<LX_INIT_GUEST_CAPABILITIES>(nullptr, 30 * 1000);
    auto notifications = listener.Accept(30 * 1000);
    return {std::move(channel), std::move(notifications)};
}

template <typename Callback>
HRESULT OperationResult(Callback&& Operation)
{
    return wil::ResultFromException(std::forward<Callback>(Operation));
}

inline void VerifyBootsAndTerminates(std::unique_ptr<IVirtualMachineBackend> Backend)
{
    auto terminationEvent = Backend->GetTerminationEvent();
    auto crashEvent = Backend->GetCrashEvent();
    VERIFY_IS_FALSE(Backend->GetCrashLogPath().has_value());
    VERIFY_ARE_EQUAL(VmState::Created, Backend->GetState());
    VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), OperationResult([&] { Backend->GetTerminationReason(); }));
    auto guest = StartGuest(*Backend);
    VERIFY_ARE_EQUAL(VmState::Running, Backend->GetState());

    const auto runningResult = WaitForSingleObject(terminationEvent.get(), 100);
    VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), runningResult);
    VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(crashEvent.get(), 0));
    if (runningResult == WAIT_TIMEOUT)
    {
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), OperationResult([&] { Backend->GetTerminationReason(); }));
        Backend->Terminate();
    }

    VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(terminationEvent.get(), 30 * 1000));
    VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(crashEvent.get(), 0));
    VERIFY_ARE_EQUAL(VmState::Stopped, Backend->GetState());
    const auto terminationInformation = Backend->GetTerminationReason();
    VERIFY_ARE_EQUAL(VmTerminationReason::Shutdown, terminationInformation.Reason);
    VERIFY_IS_FALSE(terminationInformation.Details.empty());
    const auto repeatedInformation = Backend->GetTerminationReason();
    VERIFY_ARE_EQUAL(terminationInformation.Reason, repeatedInformation.Reason);
    VERIFY_ARE_EQUAL(terminationInformation.Details, repeatedInformation.Details);
}

} // namespace VirtualMachineBackendTestHelpers
