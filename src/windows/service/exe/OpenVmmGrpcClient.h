// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "VMService.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>

namespace wsl::windows::service {

class OpenVmmGrpcClient
{
public:
    HRESULT Connect(_In_ LPCWSTR SocketPath, _In_ UINT32 TimeoutMs);
    HRESULT Disconnect();

    HRESULT SetKernelPath(_In_ LPCWSTR Path);
    HRESULT SetInitrdPath(_In_ LPCWSTR Path);
    HRESULT SetKernelCmdLine(_In_ LPCWSTR CommandLine);
    HRESULT SetMemoryMb(_In_ UINT64 MemoryMb);
    HRESULT SetProcessorCount(_In_ UINT32 Count);
    HRESULT SetHvSocketPath(_In_ LPCWSTR Path);
    HRESULT AddBootDisk(_In_ UINT32 Controller, _In_ UINT32 Lun, _In_ LPCWSTR HostPath, _In_ BOOL ReadOnly);
    HRESULT SetConsommeNic(_In_ LPCWSTR NicId, _In_ LPCWSTR MacAddress);
    HRESULT AddSerialPort(_In_ UINT32 Port, _In_ LPCWSTR SocketPath);
    HRESULT SetVirtioConsolePath(_In_ LPCWSTR Path);

    HRESULT CreateVm();
    HRESULT ResumeVm();
    HRESULT TeardownVm();
    HRESULT Quit();

    HRESULT AttachScsiDisk(_In_ UINT32 Controller, _In_ UINT32 Lun, _In_ LPCWSTR HostPath, _In_ BOOL ReadOnly);
    HRESULT DetachScsiDisk(_In_ UINT32 Controller, _In_ UINT32 Lun);
    HRESULT AddVirtioFsDevice(_In_ LPCWSTR Tag, _In_ LPCWSTR RootPath, _Out_ GUID* InstanceId);
    HRESULT RemoveVpciDevice(_In_ const GUID& InstanceId);
    HRESULT BindPort(_In_ UINT16 HostPort, _In_ UINT16 GuestPort, _In_ BOOL Tcp, _In_ INT32 Family);
    HRESULT UnbindPort(_In_ UINT16 HostPort, _In_ UINT16 GuestPort, _In_ BOOL Tcp, _In_ INT32 Family);

private:
    static HRESULT StatusToHresult(const grpc::Status& Status);

    HRESULT ModifyNic(vmservice::ModifyType Type, UINT16 HostPort, UINT16 GuestPort, BOOL Tcp);
    HRESULT ModifyDisk(vmservice::ModifyType Type, UINT32 Controller, UINT32 Lun, LPCWSTR HostPath, BOOL ReadOnly);

    template <typename TResponse, typename TOperation>
    HRESULT Call(TResponse& Response, TOperation&& Operation) const
    {
        if (!m_stub)
        {
            return HRESULT_FROM_WIN32(ERROR_NOT_CONNECTED);
        }

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + m_timeout);
        return StatusToHresult(Operation(context, Response));
    }

    template <typename TOperation>
    HRESULT Call(TOperation&& Operation) const
    {
        google::protobuf::Empty response;
        return Call(response, std::forward<TOperation>(Operation));
    }

    std::chrono::milliseconds m_timeout{};
    vmservice::VMConfig m_config;
    std::string m_nicId;
    std::string m_macAddress;
    std::unique_ptr<vmservice::VM::Stub> m_stub;
};

} // namespace wsl::windows::service