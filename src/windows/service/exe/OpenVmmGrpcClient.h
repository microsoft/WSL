// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "VMService.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <memory>
#include <string>

namespace wsl::windows::service::wslc {

class OpenVmmGrpcClient
{
public:
    HRESULT Connect(LPCWSTR socketPath, UINT32 timeoutMs);
    HRESULT Disconnect();
    HRESULT SetKernelPath(LPCWSTR path);
    HRESULT SetInitrdPath(LPCWSTR path);
    HRESULT SetKernelCmdLine(LPCWSTR commandLine);
    HRESULT SetMemoryMb(UINT64 memoryMb);
    HRESULT SetProcessorCount(UINT32 count);
    HRESULT SetHvSocketPath(LPCWSTR path);
    HRESULT AddBootDisk(UINT32 controller, UINT32 lun, LPCWSTR hostPath, BOOL readOnly);
    HRESULT SetConsommeNic(LPCWSTR nicId, LPCWSTR macAddress);
    HRESULT AddSerialPort(UINT32 port, LPCWSTR socketPath);
    HRESULT SetVirtioConsolePath(LPCWSTR path);
    HRESULT CreateVm();
    HRESULT ResumeVm();
    HRESULT TeardownVm();
    HRESULT Quit();
    HRESULT AttachScsiDisk(UINT32 controller, UINT32 lun, LPCWSTR hostPath, BOOL readOnly);
    HRESULT DetachScsiDisk(UINT32 controller, UINT32 lun);
    HRESULT AddShare(LPCWSTR tag, LPCWSTR hostPath, BOOL readOnly);
    HRESULT RemoveShare(LPCWSTR tag);
    HRESULT BindPort(UINT16 hostPort, UINT16 guestPort, BOOL tcp, INT32 family);
    HRESULT UnbindPort(UINT16 hostPort, UINT16 guestPort, BOOL tcp, INT32 family);

private:
    static std::string ToUtf8(LPCWSTR value);
    static HRESULT StatusToHresult(const grpc::Status& status);
    HRESULT ModifyNic(vmservice::ModifyType type, UINT16 hostPort, UINT16 guestPort, BOOL tcp);
    HRESULT ModifyDisk(vmservice::ModifyType type, UINT32 controller, UINT32 lun, LPCWSTR hostPath, BOOL readOnly);

    template <typename TOperation>
    HRESULT Call(TOperation&& operation) const
    {
        if (!m_stub)
        {
            return HRESULT_FROM_WIN32(ERROR_NOT_CONNECTED);
        }

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + m_timeout);
        google::protobuf::Empty response;
        return StatusToHresult(operation(context, response));
    }

    std::chrono::milliseconds m_timeout{};
    vmservice::VMConfig m_config;
    std::string m_nicId;
    std::string m_macAddress;
    std::unique_ptr<vmservice::VM::Stub> m_stub;
};

} // namespace wsl::windows::service::wslc